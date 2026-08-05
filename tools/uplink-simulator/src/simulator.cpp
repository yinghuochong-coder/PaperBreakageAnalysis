#include "paperbreak/uplink/simulator.hpp"

#include "paperbreak/common/error.hpp"
#include "paperbreak/uplink/protocol.hpp"
#include "workspace.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QHttpServer>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QHttpServerWebSocketUpgradeResponse>
#include <QRegularExpression>
#include <QTcpServer>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QWebSocket>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

namespace paperbreak::uplink::simulator
{
namespace
{
using Json = nlohmann::json;
using HttpStatus = QHttpServerResponse::StatusCode;

Error simulator_error(std::string code, std::string message, std::string operation,
                      const bool retryable = false)
{
    return make_error(std::move(code), Severity::error, std::move(message), "uplink-simulator",
                      std::move(operation), retryable);
}

std::string error_json(const Error& error)
{
    Json details = Json::object();
    for (const auto& detail : error.details)
        details[detail.key] = detail.value;
    Json value{{"success", false},
               {"error",
                {{"businessCode", error.business_code},
                 {"severity", error.severity == Severity::critical  ? "Critical"
                              : error.severity == Severity::warning ? "Warning"
                              : error.severity == Severity::info    ? "Info"
                                                                    : "Error"},
                 {"message", error.message},
                 {"module", error.module},
                 {"operation", error.operation},
                 {"details", std::move(details)},
                 {"retryable", error.retryable},
                 {"timestamp", error.timestamp}}}};
    if (error.correlation_id)
        value["error"]["correlationId"] = *error.correlation_id;
    return value.dump();
}

HttpStatus status_for(const Error& error)
{
    if (error.business_code == "UPLINK_PROTOCOL_VERSION_UNSUPPORTED")
        return HttpStatus::UpgradeRequired;
    if (error.business_code == "UPLINK_PROTOCOL_ERROR")
        return error.operation.find("idempotency") != std::string::npos ? HttpStatus::Conflict
                                                                        : HttpStatus::BadRequest;
    if (error.business_code == "UPLINK_SERVER_BUSY")
        return HttpStatus::ServiceUnavailable;
    if (error.business_code == "UPLOAD_CHECKSUM_MISMATCH")
        return HttpStatus::UnprocessableEntity;
    if (error.business_code == "UPLOAD_REJECTED")
        return HttpStatus::Conflict;
    if (error.business_code == "UPLOAD_TRANSFER_FAILED" &&
        error.operation == "simulator.upload.chunk.capacity")
        return HttpStatus::InsufficientStorage;
    if (error.business_code.starts_with("DATABASE_"))
        return HttpStatus::InternalServerError;
    return HttpStatus::BadRequest;
}

QHttpServerResponse json_response(const std::string& body, const HttpStatus status = HttpStatus::Ok)
{
    return QHttpServerResponse("application/json; charset=utf-8",
                               QByteArray(body.data(), static_cast<qsizetype>(body.size())),
                               status);
}

template <typename T>
QHttpServerResponse result_response(const Result<T>& result,
                                    const HttpStatus success_status = HttpStatus::Ok)
{
    if (!result)
        return json_response(error_json(result.error()), status_for(result.error()));
    if constexpr (std::is_same_v<T, std::string>)
        return json_response(result.value(), success_status);
    else
        return json_response(Json{{"success", true}}.dump(), success_status);
}

std::string uuid()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
}

std::string request_body(const QHttpServerRequest& request)
{
    const QByteArray body = request.body();
    return {body.constData(), static_cast<std::size_t>(body.size())};
}

std::vector<std::byte> request_bytes(const QHttpServerRequest& request)
{
    const QByteArray body = request.body();
    const auto* begin = reinterpret_cast<const std::byte*>(body.constData());
    return {begin, begin + body.size()};
}

bool dangerous_command(const std::string_view type)
{
    return type != "system.requestStatus";
}

const std::unordered_set<std::string>& allowed_commands()
{
    static const std::unordered_set<std::string> commands{
        "system.requestStatus",   "config.replace", "event.review",        "event.retryUpload",
        "camera.discover",        "camera.bind",    "camera.connect",      "camera.disconnect",
        "camera.start",           "camera.stop",    "camera.updateConfig", "camera.captureSnapshot",
        "camera.softwareTrigger", "service.restart"};
    return commands;
}

} // namespace

class Runtime::Impl final
{
  private:
    class StorageWorker final
    {
      public:
        Result<WorkspaceReport> start(const Options& options)
        {
            std::scoped_lock lock(mutex_);
            if (thread_.joinable())
                return Result<WorkspaceReport>::failure(simulator_error(
                    "SYS_ALREADY_RUNNING", "模拟器存储线程已运行", "simulator.storage.start"));
            auto started = std::make_shared<std::promise<Result<WorkspaceReport>>>();
            auto future = started->get_future();
            operation_stop_ = std::stop_source{};
            const std::stop_token operation_stop = operation_stop_.get_token();
            thread_ =
                std::jthread([this, options, started, operation_stop](const std::stop_token stop) {
                    Workspace workspace;
                    auto opened = workspace.open(options.workspace, options.workspace_limit_bytes,
                                                 options.maximum_device_count, operation_stop);
                    started->set_value(opened);
                    if (!opened)
                        return;
                    {
                        std::scoped_lock state_lock(mutex_);
                        ready_ = true;
                        used_bytes_.store(workspace.used_bytes());
                    }
                    condition_.notify_all();
                    while (true)
                    {
                        std::function<void(Workspace&)> task;
                        {
                            std::unique_lock queue_lock(mutex_);
                            condition_.wait(queue_lock, stop, [this, &stop] {
                                return !queue_.empty() || stop.stop_requested();
                            });
                            if (queue_.empty() && stop.stop_requested())
                                break;
                            if (queue_.empty())
                                continue;
                            task = std::move(queue_.front());
                            queue_.pop_front();
                        }
                        task(workspace);
                        used_bytes_.store(workspace.used_bytes());
                    }
                    workspace.close();
                    std::scoped_lock state_lock(mutex_);
                    ready_ = false;
                });
            auto result = future.get();
            if (!result && thread_.joinable())
                thread_.join();
            return result;
        }

        template <typename T> Result<T> call(std::function<Result<T>(Workspace&)> operation)
        {
            auto completion = std::make_shared<std::promise<Result<T>>>();
            auto future = completion->get_future();
            {
                std::scoped_lock lock(mutex_);
                if (!ready_ || !thread_.joinable())
                    return Result<T>::failure(simulator_error(
                        "DATABASE_NOT_READY", "模拟器存储线程尚未就绪", "simulator.storage.call"));
                if (queue_.size() >= storage_queue_capacity)
                {
                    ++rejected_;
                    return Result<T>::failure(simulator_error("UPLINK_SERVER_BUSY",
                                                              "模拟器存储队列已满",
                                                              "simulator.storage.call", true));
                }
                queue_.push_back(
                    [operation = std::move(operation), completion](Workspace& workspace) {
                        completion->set_value(operation(workspace));
                    });
                high_watermark_ = std::max(high_watermark_, queue_.size());
            }
            condition_.notify_one();
            return future.get();
        }

        void stop() noexcept
        {
            if (!thread_.joinable())
                return;
            thread_.request_stop();
            condition_.notify_all();
            thread_.join();
            std::scoped_lock lock(mutex_);
            queue_.clear();
            ready_ = false;
        }

        void cancel_operations() noexcept
        {
            operation_stop_.request_stop();
        }

        [[nodiscard]] std::size_t depth() const
        {
            std::scoped_lock lock(mutex_);
            return queue_.size();
        }
        [[nodiscard]] std::size_t high_watermark() const
        {
            std::scoped_lock lock(mutex_);
            return high_watermark_;
        }
        [[nodiscard]] std::size_t rejected() const noexcept
        {
            return rejected_.load();
        }
        [[nodiscard]] std::uint64_t used_bytes() const noexcept
        {
            return used_bytes_.load();
        }

      private:
        mutable std::mutex mutex_;
        std::condition_variable_any condition_;
        std::deque<std::function<void(Workspace&)>> queue_;
        std::jthread thread_;
        std::stop_source operation_stop_;
        bool ready_{};
        std::size_t high_watermark_{};
        std::atomic_size_t rejected_{};
        std::atomic_uint64_t used_bytes_{};
    };

    struct DeviceRuntime final
    {
        DeviceSnapshot snapshot;
        std::string session_id;
        std::unique_ptr<QWebSocket> socket;
        std::chrono::steady_clock::time_point last_preview{};
    };

  public:
    Result<void> start(Options options)
    {
        std::scoped_lock lifecycle_lock(lifecycle_mutex_);
        if (running_.load())
            return Result<void>::failure(
                simulator_error("SYS_ALREADY_RUNNING", "上位机模拟器已运行", "simulator.start"));
        if (options.listen_address.empty() || options.workspace.empty() ||
            options.maximum_device_count == 0U || options.maximum_device_count > maximum_devices ||
            options.workspace_limit_bytes == 0U)
            return Result<void>::failure(simulator_error(
                "SYS_CONFIG_INVALID", "上位机模拟器启动参数无效", "simulator.start"));
        options_ = std::move(options);
        stopping_.store(false);
        if (options_.scenario_path)
        {
            std::ifstream input(*options_.scenario_path, std::ios::binary);
            if (!input)
                return Result<void>::failure(simulator_error(
                    "SYS_CONFIG_INVALID", "无法打开故障场景文件", "simulator.scenario.open"));
            const std::string contents((std::istreambuf_iterator<char>(input)),
                                       std::istreambuf_iterator<char>());
            auto parsed = parse_scenario(contents);
            if (!parsed)
                return Result<void>::failure(parsed.error());
            std::scoped_lock fault_lock(fault_mutex_);
            for (auto& [machine, profile] : parsed.value())
                faults_[std::move(machine)] = profile;
        }
        auto workspace = storage_.start(options_);
        if (!workspace)
            return Result<void>::failure(workspace.error());

        {
            std::scoped_lock snapshot_lock(snapshot_mutex_);
            snapshot_ = {.running = false,
                         .listen_address = options_.listen_address,
                         .port = options_.port,
                         .workspace = options_.workspace,
                         .workspace_used_bytes = workspace.value().used_bytes,
                         .workspace_limit_bytes = options_.workspace_limit_bytes};
            append_log_locked("警告：Uplink v1 使用明文、无鉴权并默认监听全部网卡");
            append_log_locked(
                "工作区恢复：有效断点 " + std::to_string(workspace.value().recovered_uploads) +
                "，隔离异常文件 " + std::to_string(workspace.value().quarantined_uploads));
        }

        auto started = std::make_shared<std::promise<Result<std::uint16_t>>>();
        auto future = started->get_future();
        network_thread_ = std::jthread(
            [this, started](const std::stop_token stop) { run_network(stop, started); });
        auto network = future.get();
        if (!network)
        {
            network_thread_.request_stop();
            if (network_thread_.joinable())
                network_thread_.join();
            storage_.stop();
            return Result<void>::failure(network.error());
        }
        options_.port = network.value();
        running_.store(true);
        {
            std::scoped_lock snapshot_lock(snapshot_mutex_);
            snapshot_.running = true;
            snapshot_.port = network.value();
            append_log_locked("模拟器已监听 " + options_.listen_address + ":" +
                              std::to_string(network.value()));
        }
        return Result<void>::success();
    }

    void stop(const std::chrono::milliseconds timeout) noexcept
    {
        const auto deadline =
            std::chrono::steady_clock::now() + std::max(timeout, std::chrono::milliseconds::zero());
        std::scoped_lock lifecycle_lock(lifecycle_mutex_);
        stopping_.store(true);
        if (network_thread_.joinable())
        {
            network_thread_.request_stop();
            {
                std::scoped_lock context_lock(context_mutex_);
                if (network_loop_ != nullptr)
                    QMetaObject::invokeMethod(network_loop_, &QEventLoop::quit,
                                              Qt::QueuedConnection);
            }
            storage_.cancel_operations();
            network_thread_.join();
        }
        storage_.stop();
        running_.store(false);
        std::scoped_lock snapshot_lock(snapshot_mutex_);
        snapshot_.running = false;
        for (auto& device : snapshot_.devices)
            device.connected = false;
        append_log_locked("模拟器已停止，工作区 checkpoint 已保存");
        if (std::chrono::steady_clock::now() > deadline)
            append_log_locked("警告：模拟器关闭超过请求的截止时间");
    }

    bool running() const noexcept
    {
        return running_.load();
    }

    Snapshot snapshot() const
    {
        std::scoped_lock lock(snapshot_mutex_);
        Snapshot copy = snapshot_;
        copy.workspace_used_bytes = storage_.used_bytes();
        copy.storage_queue_depth = storage_.depth();
        copy.storage_queue_high_watermark = storage_.high_watermark();
        copy.rejected_storage_tasks = storage_.rejected();
        return copy;
    }

    Result<void> enqueue_command(CommandRequest command)
    {
        auto machine = validate_identifier(command.machine_id, "machineId", 64U);
        auto id = validate_identifier(command.command_id, "commandId", 128U);
        auto type = validate_identifier(command.command_type, "commandType", 128U);
        if (!machine)
            return machine;
        if (!id)
            return id;
        if (!type)
            return type;
        if (!allowed_commands().contains(command.command_type))
            return Result<void>::failure(simulator_error(
                "SYS_NOT_SUPPORTED", "模拟器不支持该远程命令类型", "simulator.command.enqueue"));
        if (command.deadline.empty() || command.deadline.size() > 64U)
            return Result<void>::failure(simulator_error(
                "UPLINK_PROTOCOL_ERROR", "命令截止时间无效", "simulator.command.enqueue"));
        auto payload = Json::parse(command.payload_json, nullptr, false);
        if (payload.is_discarded() || !payload.is_object() ||
            command.payload_json.size() > maximum_json_message_bytes)
            return Result<void>::failure(simulator_error("UPLINK_PROTOCOL_ERROR",
                                                         "命令 payload 必须是有界 JSON 对象",
                                                         "simulator.command.enqueue"));
        if (dangerous_command(command.command_type) && !command.operator_confirmed)
            return Result<void>::failure(simulator_error("SYS_PERMISSION_DENIED",
                                                         "危险远程命令尚未获得操作员二次确认",
                                                         "simulator.command.enqueue"));
        {
            std::scoped_lock snapshot_lock(snapshot_mutex_);
            const auto iterator = std::ranges::find(snapshot_.devices, command.machine_id,
                                                    &DeviceSnapshot::machine_id);
            if (iterator == snapshot_.devices.end())
                return Result<void>::failure(simulator_error("UPLINK_DISCONNECTED",
                                                             "目标边缘设备尚未建立会话",
                                                             "simulator.command.enqueue", true));
            if (std::ranges::find(iterator->capabilities, command.command_type) ==
                iterator->capabilities.end())
                return Result<void>::failure(simulator_error("SYS_NOT_SUPPORTED",
                                                             "目标边缘设备未声明该命令能力",
                                                             "simulator.command.capability"));
        }
        {
            std::scoped_lock queue_lock(command_mutex_);
            auto& queue = commands_[command.machine_id];
            const auto duplicate =
                std::ranges::find_if(queue, [&command](const CommandRequest& item) {
                    return item.command_id == command.command_id;
                });
            if (duplicate != queue.end())
            {
                const bool same = duplicate->command_type == command.command_type &&
                                  duplicate->deadline == command.deadline &&
                                  duplicate->payload_json == command.payload_json &&
                                  duplicate->operator_confirmed == command.operator_confirmed;
                return same ? Result<void>::success()
                            : Result<void>::failure(simulator_error(
                                  "UPLINK_PROTOCOL_ERROR", "commandId 对应内容发生冲突",
                                  "simulator.command.idempotency"));
            }
            if (queue.size() >= command_queue_capacity_per_device)
                return Result<void>::failure(simulator_error("UPLINK_SERVER_BUSY",
                                                             "目标设备命令队列已满",
                                                             "simulator.command.enqueue", true));
            auto stored = storage_.call<void>([&command](Workspace& workspace) {
                return workspace.store_command(command.command_id, command.machine_id,
                                               command.command_type, command.payload_json,
                                               command.deadline);
            });
            if (!stored)
                return stored;
            queue.push_back(std::move(command));
        }
        return Result<void>::success();
    }

    Result<void> set_fault_profile(std::string machine_id, const FaultProfile profile)
    {
        auto valid = validate_identifier(machine_id, "machineId", 64U);
        if (!valid)
            return valid;
        if (profile.response_delay_ms > 60000U || profile.fail_next_requests > 10000U)
            return Result<void>::failure(simulator_error(
                "SYS_CONFIG_INVALID", "故障注入参数超过上限", "simulator.fault.update"));
        std::scoped_lock lock(fault_mutex_);
        faults_[std::move(machine_id)] = profile;
        return Result<void>::success();
    }

  private:
    void append_log_locked(std::string message)
    {
        snapshot_.recent_logs.push_back(current_utc_timestamp() + " " + std::move(message));
        if (snapshot_.recent_logs.size() > 200U)
            snapshot_.recent_logs.erase(
                snapshot_.recent_logs.begin(),
                snapshot_.recent_logs.begin() +
                    static_cast<std::ptrdiff_t>(snapshot_.recent_logs.size() - 200U));
    }

    FaultProfile fault_for(const std::string_view machine_id)
    {
        std::scoped_lock lock(fault_mutex_);
        const auto iterator = faults_.find(std::string{machine_id});
        return iterator == faults_.end() ? FaultProfile{} : iterator->second;
    }

    bool apply_request_fault(const std::string_view machine_id, QHttpServerResponse& response)
    {
        FaultProfile profile;
        bool fail = false;
        {
            std::scoped_lock lock(fault_mutex_);
            auto iterator = faults_.find(std::string{machine_id});
            if (iterator == faults_.end())
                return false;
            profile = iterator->second;
            if (iterator->second.fail_next_requests > 0U)
            {
                --iterator->second.fail_next_requests;
                fail = true;
            }
        }
        std::uint32_t remaining_delay = profile.response_delay_ms;
        while (remaining_delay > 0U && !stopping_.load())
        {
            const std::uint32_t slice = std::min(remaining_delay, 10U);
            QThread::msleep(slice);
            remaining_delay -= slice;
        }
        if (profile.reject_connections || fail)
        {
            auto error = simulator_error("UPLINK_SERVER_BUSY", "故障场景拒绝本次请求",
                                         "simulator.fault.request", true);
            response = json_response(error_json(error), HttpStatus::ServiceUnavailable);
            return true;
        }
        return false;
    }

    void refresh_snapshot(const std::map<std::string, DeviceRuntime>& devices)
    {
        std::scoped_lock lock(snapshot_mutex_);
        snapshot_.devices.clear();
        snapshot_.devices.reserve(devices.size());
        for (const auto& [machine, device] : devices)
        {
            DeviceSnapshot copy = device.snapshot;
            std::scoped_lock queue_lock(command_mutex_);
            const auto queue = commands_.find(machine);
            copy.pending_commands = queue == commands_.end() ? 0U : queue->second.size();
            snapshot_.devices.push_back(std::move(copy));
        }
        auto uploads = storage_.call<std::vector<StoredUploadSnapshot>>(
            [](Workspace& workspace) { return workspace.upload_snapshots(); });
        if (uploads)
        {
            snapshot_.uploads.clear();
            snapshot_.uploads.reserve(uploads.value().size());
            for (const auto& upload : uploads.value())
                snapshot_.uploads.push_back({.upload_id = upload.upload_id,
                                             .machine_id = upload.machine_id,
                                             .event_id = upload.event_id,
                                             .logical_file_id = upload.logical_file_id,
                                             .state = upload.state,
                                             .received_bytes = upload.received_bytes,
                                             .total_bytes = upload.total_bytes});
        }
        snapshot_.workspace_used_bytes = storage_.used_bytes();
    }

    void run_network(const std::stop_token stop,
                     const std::shared_ptr<std::promise<Result<std::uint16_t>>>& started)
    {
        QEventLoop loop;
        {
            std::scoped_lock lock(context_mutex_);
            network_loop_ = &loop;
        }
        QHttpServer server;
        std::map<std::string, DeviceRuntime> devices;

        server.route("/healthz", QHttpServerRequest::Method::Get, [this] {
            return json_response(Json{{"status", "ready"},
                                      {"protocolVersion", protocol_version},
                                      {"security", "plaintext-unauthenticated"}}
                                     .dump());
        });

        server.route("/api/uplink/v1/sessions", QHttpServerRequest::Method::Post,
                     [this, &devices](const QHttpServerRequest& request) {
                         auto hello = parse_session_hello(request_body(request));
                         if (!hello)
                             return result_response(hello);
                         if (std::ranges::find(hello.value().supported_protocol_versions,
                                               protocol_version) ==
                             hello.value().supported_protocol_versions.end())
                         {
                             auto error = simulator_error("UPLINK_PROTOCOL_VERSION_UNSUPPORTED",
                                                          "边缘设备不支持 Uplink v1",
                                                          "simulator.session.negotiate");
                             return json_response(error_json(error), HttpStatus::UpgradeRequired);
                         }
                         QHttpServerResponse fault(HttpStatus::Ok);
                         if (apply_request_fault(hello.value().machine_id, fault))
                             return fault;
                         const std::string session_id = uuid();
                         const std::string websocket_host =
                             request.localAddress().toString().toStdString();
                         auto stored = storage_.call<StoredSession>(
                             [&hello, &session_id, websocket_host, this](Workspace& workspace) {
                                 return workspace.create_session(hello.value(), session_id,
                                                                 current_utc_timestamp(),
                                                                 websocket_host, options_.port);
                             });
                         if (!stored)
                             return result_response(stored);
                         auto& device = devices[hello.value().machine_id];
                         device.snapshot.machine_id = hello.value().machine_id;
                         device.snapshot.production_line_id = hello.value().production_line_id;
                         device.snapshot.software_version = hello.value().software_version;
                         device.snapshot.last_seen = current_utc_timestamp();
                         device.snapshot.capabilities = hello.value().capabilities;
                         device.session_id = stored.value().session_id;
                         return json_response(stored.value().response_json,
                                              stored.value().duplicate ? HttpStatus::Ok
                                                                       : HttpStatus::Created);
                     });

        server.route("/api/uplink/v1/devices/<arg>/events/<arg>", QHttpServerRequest::Method::Put,
                     [this, &devices](const QString& machine, const QString& event,
                                      const QHttpServerRequest& request) {
                         const std::string machine_id = machine.toStdString();
                         QHttpServerResponse fault(HttpStatus::Ok);
                         if (apply_request_fault(machine_id, fault))
                             return fault;
                         const std::string event_id = event.toStdString();
                         const std::string body = request_body(request);
                         auto stored = storage_.call<std::string>(
                             [machine_id, event_id, body](Workspace& workspace) {
                                 return workspace.store_event(machine_id, event_id, body);
                             });
                         if (stored)
                             ++devices[machine_id].snapshot.event_count;
                         return result_response(stored, HttpStatus::Accepted);
                     });

        server.route(
            "/api/uplink/v1/devices/<arg>/uploads", QHttpServerRequest::Method::Post,
            [this, &devices](const QString& machine, const QHttpServerRequest& request) {
                const std::string machine_id = machine.toStdString();
                QHttpServerResponse fault(HttpStatus::Ok);
                if (apply_request_fault(machine_id, fault))
                    return fault;
                auto parsed = parse_upload_create(request_body(request));
                if (!parsed)
                    return result_response(parsed);
                auto active_count = storage_.call<std::size_t>(
                    [](Workspace& workspace) { return workspace.active_upload_count(); });
                if (!active_count)
                    return result_response(active_count);
                if (active_count.value() >= 32U)
                {
                    auto error =
                        simulator_error("UPLINK_SERVER_BUSY", "模拟器已达到 32 个活动上传上限",
                                        "simulator.upload.create", true);
                    return json_response(error_json(error), HttpStatus::ServiceUnavailable);
                }
                const std::string upload_id = uuid();
                auto stored = storage_.call<StoredUpload>(
                    [machine_id, &parsed, upload_id](Workspace& workspace) {
                        return workspace.create_upload(machine_id, parsed.value(), upload_id);
                    });
                if (stored)
                {
                    ++devices[machine_id].snapshot.upload_count;
                }
                if (!stored)
                    return result_response(stored);
                return json_response(stored.value().response_json, stored.value().duplicate
                                                                       ? HttpStatus::Ok
                                                                       : HttpStatus::Created);
            });

        server.route("/api/uplink/v1/devices/<arg>/uploads/<arg>", QHttpServerRequest::Method::Get,
                     [this](const QString& machine, const QString& upload) {
                         const std::string machine_id = machine.toStdString();
                         const std::string upload_id = upload.toStdString();
                         auto status = storage_.call<std::string>(
                             [machine_id, upload_id](Workspace& workspace) {
                                 return workspace.upload_status(machine_id, upload_id);
                             });
                         return result_response(status);
                     });

        server.route(
            "/api/uplink/v1/devices/<arg>/uploads/<arg>/chunks/<arg>",
            QHttpServerRequest::Method::Put,
            [this](const QString& machine, const QString& upload, const quint32 chunk,
                   const QHttpServerRequest& request) {
                const std::string machine_id = machine.toStdString();
                QHttpServerResponse fault(HttpStatus::Ok);
                if (apply_request_fault(machine_id, fault))
                    return fault;
                const std::string upload_id = upload.toStdString();
                const std::string digest = request.value("x-chunk-sha256").toStdString();
                const std::string content_range = request.value("content-range").toStdString();
                const auto bytes = request_bytes(request);
                const FaultProfile profile = fault_for(machine_id);
                auto stored = storage_.call<std::string>(
                    [machine_id, upload_id, chunk, digest, content_range, bytes,
                     mismatch = profile.force_checksum_mismatch](Workspace& workspace) {
                        return workspace.store_chunk(machine_id, upload_id, chunk, digest,
                                                     content_range, bytes, mismatch);
                    });
                if (profile.disconnect_after_chunk && *profile.disconnect_after_chunk == chunk)
                {
                    auto error =
                        simulator_error("UPLOAD_TRANSFER_FAILED", "故障场景在指定分块后中断请求",
                                        "simulator.fault.chunk", true);
                    return json_response(error_json(error), HttpStatus::ServiceUnavailable);
                }
                return result_response(stored, HttpStatus::Accepted);
            });

        server.route("/api/uplink/v1/devices/<arg>/uploads/<arg>/complete",
                     QHttpServerRequest::Method::Post,
                     [this](const QString& machine, const QString& upload) {
                         const std::string machine_id = machine.toStdString();
                         const std::string upload_id = upload.toStdString();
                         QHttpServerResponse fault(HttpStatus::Ok);
                         if (apply_request_fault(machine_id, fault))
                             return fault;
                         const FaultProfile profile = fault_for(machine_id);
                         auto completed = storage_.call<std::string>(
                             [machine_id, upload_id,
                              mismatch = profile.force_checksum_mismatch](Workspace& workspace) {
                                 return workspace.complete_upload(machine_id, upload_id, mismatch);
                             });
                         return result_response(completed);
                     });

        server.addWebSocketUpgradeVerifier(&server, [](const QHttpServerRequest& request) {
            static const QRegularExpression pattern(
                QStringLiteral("^/api/uplink/v1/sessions/[A-Za-z0-9._-]+/stream$"));
            return pattern.match(request.url().path()).hasMatch()
                       ? QHttpServerWebSocketUpgradeResponse::accept()
                       : QHttpServerWebSocketUpgradeResponse::deny(404,
                                                                   "Unknown WebSocket endpoint");
        });

        QObject::connect(
            &server, &QAbstractHttpServer::newWebSocketConnection, &server,
            [this, &server, &devices] {
                auto socket = server.nextPendingWebSocketConnection();
                if (!socket)
                    return;
                const QStringList segments =
                    socket->requestUrl().path().split('/', Qt::SkipEmptyParts);
                if (segments.size() != 6)
                {
                    socket->close(QWebSocketProtocol::CloseCodeProtocolError,
                                  "Invalid session path");
                    return;
                }
                const std::string session_id = segments[4].toStdString();
                auto machine = storage_.call<std::string>([session_id](Workspace& workspace) {
                    return workspace.session_machine(session_id);
                });
                if (!machine)
                {
                    socket->close(QWebSocketProtocol::CloseCodePolicyViolated, "Unknown session");
                    return;
                }
                const FaultProfile profile = fault_for(machine.value());
                if (profile.reject_connections || profile.disconnect_websockets)
                {
                    socket->close(QWebSocketProtocol::CloseCodePolicyViolated,
                                  "Injected disconnect");
                    return;
                }
                auto& device = devices[machine.value()];
                if (device.socket)
                    device.socket->close(QWebSocketProtocol::CloseCodeGoingAway,
                                         "Superseded by new session");
                device.session_id = session_id;
                device.snapshot.machine_id = machine.value();
                device.snapshot.connected = true;
                device.snapshot.last_seen = current_utc_timestamp();
                socket->setMaxAllowedIncomingMessageSize(4U + maximum_preview_header_bytes +
                                                         maximum_preview_jpeg_bytes);
                socket->setMaxAllowedIncomingFrameSize(4U + maximum_preview_header_bytes +
                                                       maximum_preview_jpeg_bytes);
                QWebSocket* raw = socket.get();
                QObject::connect(
                    raw, &QWebSocket::textMessageReceived, raw,
                    [this, raw, &devices, machine_id = machine.value()](const QString& text) {
                        auto message = parse_message_envelope(text.toStdString());
                        if (!message || message.value().machine_id != machine_id)
                        {
                            raw->close(QWebSocketProtocol::CloseCodeProtocolError,
                                       "Invalid v1 envelope");
                            return;
                        }
                        auto stored = storage_.call<void>([&message](Workspace& workspace) {
                            return workspace.store_message(message.value());
                        });
                        if (!stored)
                        {
                            raw->close(QWebSocketProtocol::CloseCodeBadOperation,
                                       "Persistence failed");
                            return;
                        }
                        auto& device = devices[machine_id];
                        ++device.snapshot.received_messages;
                        device.snapshot.last_message_type = message.value().message_type;
                        if (message.value().message_type == "alarm")
                            ++device.snapshot.alarm_count;
                        device.snapshot.last_seen = current_utc_timestamp();
                        if (message.value().message_type == "command.result")
                        {
                            auto payload =
                                Json::parse(message.value().payload_json, nullptr, false);
                            if (payload.is_object() && payload.contains("commandId") &&
                                payload["commandId"].is_string())
                            {
                                const std::string command_id =
                                    payload["commandId"].get<std::string>();
                                static_cast<void>(storage_.call<void>(
                                    [command_id, payload = payload.dump()](Workspace& workspace) {
                                        return workspace.complete_command(command_id, payload);
                                    }));
                            }
                        }
                        MessageEnvelope ack{
                            .protocol_version = protocol_version,
                            .message_type = "ack",
                            .message_id = uuid(),
                            .machine_id = machine_id,
                            .sequence = message.value().sequence,
                            .timestamp = current_utc_timestamp(),
                            .payload_json =
                                Json{{"acknowledgedMessageId", message.value().message_id}}.dump()};
                        auto encoded = serialize_message_envelope(ack);
                        if (encoded)
                        {
                            raw->sendTextMessage(QString::fromStdString(encoded.value()));
                            if (fault_for(machine_id).duplicate_acknowledgements)
                                raw->sendTextMessage(QString::fromStdString(encoded.value()));
                        }
                    });
                QObject::connect(
                    raw, &QWebSocket::binaryMessageReceived, raw,
                    [this, raw, &devices, machine_id = machine.value()](const QByteArray& bytes) {
                        const auto* begin = reinterpret_cast<const std::byte*>(bytes.constData());
                        auto frame =
                            decode_preview_frame({begin, static_cast<std::size_t>(bytes.size())});
                        if (!frame || frame.value().machine_id != machine_id)
                        {
                            raw->close(QWebSocketProtocol::CloseCodeProtocolError,
                                       "Invalid preview frame");
                            return;
                        }
                        auto& device = devices[machine_id];
                        const auto now = std::chrono::steady_clock::now();
                        if (device.last_preview.time_since_epoch().count() != 0 &&
                            now - device.last_preview < std::chrono::milliseconds{200})
                        {
                            ++device.snapshot.overwritten_previews;
                            return;
                        }
                        if (!device.snapshot.latest_preview_jpeg.empty())
                            ++device.snapshot.overwritten_previews;
                        device.last_preview = now;
                        ++device.snapshot.received_previews;
                        device.snapshot.preview_camera_id = frame.value().camera_id;
                        device.snapshot.latest_preview_jpeg = std::move(frame).value().jpeg;
                        device.snapshot.last_seen = current_utc_timestamp();
                    });
                QObject::connect(
                    raw, &QWebSocket::disconnected, raw,
                    [this, &devices, machine_id = machine.value(), session_id] {
                        auto iterator = devices.find(machine_id);
                        if (iterator != devices.end())
                        {
                            iterator->second.snapshot.connected = false;
                            iterator->second.snapshot.last_seen = current_utc_timestamp();
                        }
                        static_cast<void>(storage_.call<void>([session_id](Workspace& workspace) {
                            return workspace.close_session(session_id, current_utc_timestamp());
                        }));
                    });
                device.socket = std::move(socket);
            });

        QTimer maintenance;
        maintenance.setInterval(50);
        QObject::connect(&maintenance, &QTimer::timeout, &server, [this, &devices, &loop, stop] {
            if (stop.stop_requested())
            {
                loop.quit();
                return;
            }
            {
                std::scoped_lock queue_lock(command_mutex_);
                for (auto& [machine_id, queue] : commands_)
                {
                    auto device = devices.find(machine_id);
                    if (device != devices.end() && device->second.socket &&
                        fault_for(machine_id).disconnect_websockets)
                    {
                        device->second.socket->close(QWebSocketProtocol::CloseCodeGoingAway,
                                                     "Injected WebSocket interruption");
                        continue;
                    }
                    if (queue.empty() || device == devices.end() || !device->second.socket ||
                        device->second.socket->state() != QAbstractSocket::ConnectedState)
                        continue;
                    const auto& command = queue.front();
                    auto body = Json::parse(command.payload_json, nullptr, false);
                    MessageEnvelope envelope{
                        .protocol_version = protocol_version,
                        .message_type = "command",
                        .message_id = command.command_id,
                        .machine_id = machine_id,
                        .sequence = 0U,
                        .timestamp = current_utc_timestamp(),
                        .payload_json = Json{{"commandId", command.command_id},
                                             {"commandType", command.command_type},
                                             {"deadline", command.deadline},
                                             {"operatorConfirmed", command.operator_confirmed},
                                             {"body", std::move(body)}}
                                            .dump()};
                    auto encoded = serialize_message_envelope(envelope);
                    if (encoded)
                    {
                        device->second.socket->sendTextMessage(
                            QString::fromStdString(encoded.value()));
                        if (fault_for(machine_id).replay_commands)
                            device->second.socket->sendTextMessage(
                                QString::fromStdString(encoded.value()));
                        queue.pop_front();
                    }
                }
            }
            refresh_snapshot(devices);
        });
        maintenance.start();

        QHostAddress address;
        if (!address.setAddress(QString::fromStdString(options_.listen_address)))
        {
            started->set_value(Result<std::uint16_t>::failure(simulator_error(
                "SYS_CONFIG_INVALID", "模拟器监听地址无效", "simulator.network.listen")));
            std::scoped_lock lock(context_mutex_);
            network_loop_ = nullptr;
            return;
        }
        QTcpServer tcp_server;
        if (!tcp_server.listen(address, options_.port) || !server.bind(&tcp_server))
        {
            started->set_value(Result<std::uint16_t>::failure(
                simulator_error("UPLINK_DISCONNECTED", "模拟器无法绑定监听端口",
                                "simulator.network.listen", true)));
            std::scoped_lock lock(context_mutex_);
            network_loop_ = nullptr;
            return;
        }
        const quint16 bound_port = tcp_server.serverPort();
        options_.port = bound_port;
        started->set_value(Result<std::uint16_t>::success(bound_port));
        loop.exec();

        maintenance.stop();
        for (auto& [machine_id, device] : devices)
        {
            if (device.socket)
            {
                QObject::disconnect(device.socket.get(), nullptr, nullptr, nullptr);
                device.socket->close(QWebSocketProtocol::CloseCodeGoingAway, "Simulator stopping");
            }
            if (!device.session_id.empty())
            {
                const std::string session_id = device.session_id;
                static_cast<void>(storage_.call<void>([session_id](Workspace& workspace) {
                    return workspace.close_session(session_id, current_utc_timestamp());
                }));
            }
            device.snapshot.connected = false;
        }
        refresh_snapshot(devices);
        {
            std::scoped_lock lock(context_mutex_);
            network_loop_ = nullptr;
        }
    }

    mutable std::mutex lifecycle_mutex_;
    mutable std::mutex snapshot_mutex_;
    mutable std::mutex context_mutex_;
    mutable std::mutex fault_mutex_;
    mutable std::mutex command_mutex_;
    Options options_;
    Snapshot snapshot_;
    std::unordered_map<std::string, FaultProfile> faults_;
    std::unordered_map<std::string, std::deque<CommandRequest>> commands_;
    StorageWorker storage_;
    std::jthread network_thread_;
    QEventLoop* network_loop_{};
    std::atomic_bool running_{};
    std::atomic_bool stopping_{};
};

Runtime::Runtime() : impl_(std::make_unique<Impl>()) {}
Runtime::~Runtime()
{
    impl_->stop(std::chrono::seconds{10});
}

Result<void> Runtime::start(Options options)
{
    return impl_->start(std::move(options));
}

void Runtime::stop(const std::chrono::milliseconds timeout) noexcept
{
    impl_->stop(timeout);
}

bool Runtime::running() const noexcept
{
    return impl_->running();
}

Snapshot Runtime::snapshot() const
{
    return impl_->snapshot();
}

Result<void> Runtime::enqueue_command(CommandRequest command)
{
    return impl_->enqueue_command(std::move(command));
}

Result<void> Runtime::set_fault_profile(std::string machine_id, const FaultProfile profile)
{
    return impl_->set_fault_profile(std::move(machine_id), profile);
}

Result<std::vector<std::pair<std::string, FaultProfile>>> parse_scenario(
    const std::string_view json) noexcept
{
    try
    {
        if (json.empty() || json.size() > maximum_json_message_bytes)
            return Result<std::vector<std::pair<std::string, FaultProfile>>>::failure(
                simulator_error("SYS_CONFIG_INVALID", "故障场景为空或超过 1 MiB",
                                "simulator.scenario.parse"));
        const auto value = Json::parse(json, nullptr, false);
        if (value.is_discarded() || !value.is_object() || value.size() > 2U ||
            !value.contains("schemaVersion") || value["schemaVersion"] != 1U ||
            !value.contains("devices") || !value["devices"].is_object() ||
            value["devices"].size() > maximum_devices)
            return Result<std::vector<std::pair<std::string, FaultProfile>>>::failure(
                simulator_error("SYS_CONFIG_INVALID", "故障场景必须是 schema v1 有界对象",
                                "simulator.scenario.parse"));
        std::vector<std::pair<std::string, FaultProfile>> result;
        for (const auto& [machine_id, profile] : value["devices"].items())
        {
            auto valid = validate_identifier(machine_id, "machineId", 64U);
            if (!valid || !profile.is_object())
                return Result<std::vector<std::pair<std::string, FaultProfile>>>::failure(
                    valid ? simulator_error("SYS_CONFIG_INVALID", "设备故障配置必须是对象",
                                            "simulator.scenario.parse")
                          : valid.error());
            const std::set<std::string> allowed{
                "rejectConnections", "disconnectWebSockets",  "duplicateAcknowledgements",
                "replayCommands",    "forceChecksumMismatch", "responseDelayMs",
                "failNextRequests",  "disconnectAfterChunk"};
            if (!std::ranges::all_of(profile.items(), [&allowed](const auto& item) {
                    return allowed.contains(item.key());
                }))
                return Result<std::vector<std::pair<std::string, FaultProfile>>>::failure(
                    simulator_error("SYS_CONFIG_INVALID", "故障配置包含未知字段",
                                    "simulator.scenario.parse"));
            FaultProfile parsed{
                .reject_connections = profile.value("rejectConnections", false),
                .disconnect_websockets = profile.value("disconnectWebSockets", false),
                .duplicate_acknowledgements = profile.value("duplicateAcknowledgements", false),
                .replay_commands = profile.value("replayCommands", false),
                .force_checksum_mismatch = profile.value("forceChecksumMismatch", false),
                .response_delay_ms = profile.value("responseDelayMs", 0U),
                .fail_next_requests = profile.value("failNextRequests", 0U)};
            if (profile.contains("disconnectAfterChunk"))
            {
                if (!profile["disconnectAfterChunk"].is_number_unsigned() ||
                    profile["disconnectAfterChunk"].get<std::uint64_t>() >
                        std::numeric_limits<std::uint32_t>::max())
                    return Result<std::vector<std::pair<std::string, FaultProfile>>>::failure(
                        simulator_error("SYS_CONFIG_INVALID", "disconnectAfterChunk 无效",
                                        "simulator.scenario.parse"));
                parsed.disconnect_after_chunk =
                    profile["disconnectAfterChunk"].get<std::uint32_t>();
            }
            if (parsed.response_delay_ms > 60000U || parsed.fail_next_requests > 10000U)
                return Result<std::vector<std::pair<std::string, FaultProfile>>>::failure(
                    simulator_error("SYS_CONFIG_INVALID", "故障配置超过上限",
                                    "simulator.scenario.parse"));
            result.emplace_back(machine_id, parsed);
        }
        return Result<std::vector<std::pair<std::string, FaultProfile>>>::success(
            std::move(result));
    }
    catch (...)
    {
        return Result<std::vector<std::pair<std::string, FaultProfile>>>::failure(
            simulator_error("SYS_CONFIG_INVALID", "故障场景解析异常", "simulator.scenario.parse"));
    }
}

} // namespace paperbreak::uplink::simulator
