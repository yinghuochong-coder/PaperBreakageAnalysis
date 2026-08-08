#include "paperbreak/console/operations_client.hpp"

#include <nlohmann/json.hpp>

#include <QCoreApplication>
#include <QMetaObject>
#include <QSaveFile>

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>

namespace paperbreak::console
{
namespace
{
using Json = nlohmann::json;

Error operations_error(std::string code, std::string message, std::string operation,
                       const bool retryable = false)
{
    return make_error(std::move(code), Severity::warning, std::move(message), "console",
                      std::move(operation), retryable);
}

Result<Json> response_payload(const Result<ipc::ResponseMessage>& result,
                              const std::string_view operation)
{
    if (!result)
        return Result<Json>::failure(result.error());
    if (!result.value().success)
        return Result<Json>::failure(result.value().error.value_or(operations_error(
            "IPC_PROTOCOL_ERROR", "后台服务返回未知失败", std::string{operation})));
    Json payload = Json::parse(result.value().payload_json, nullptr, false);
    if (payload.is_discarded() || !payload.is_object())
        return Result<Json>::failure(operations_error(
            "IPC_PROTOCOL_ERROR", "后台服务响应不是有效对象", std::string{operation}));
    return Result<Json>::success(std::move(payload));
}

std::string json_value_text(const Json& value)
{
    if (value.is_string())
        return value.get<std::string>();
    if (value.is_boolean())
        return value.get<bool>() ? "true" : "false";
    if (value.is_number_float())
        return std::to_string(value.get<double>());
    if (value.is_number_unsigned())
        return std::to_string(value.get<std::uint64_t>());
    if (value.is_number_integer())
        return std::to_string(value.get<std::int64_t>());
    return value.dump();
}

std::string csv_field(const std::string_view value)
{
    std::string result{"\""};
    for (const char character : value)
    {
        if (character == '"')
            result += "\"\"";
        else
            result.push_back(character);
    }
    result.push_back('"');
    return result;
}

} // namespace

class OperationsClient::FileExporter final
{
  public:
    using Completion = std::function<void(Result<std::filesystem::path>)>;

    explicit FileExporter(ThreadRegistrationFactory register_thread)
        : register_thread_(std::move(register_thread)),
          worker_([this](const std::stop_token token) { run(token); })
    {
    }
    ~FileExporter()
    {
        stop();
    }

    [[nodiscard]] Result<void> submit(std::filesystem::path destination,
                                      std::vector<std::byte> bytes, Completion completion)
    {
        std::scoped_lock lock{mutex_};
        if (stopped_)
            return Result<void>::failure(operations_error(
                "SYS_DIAGNOSTIC_EXPORT_FAILED", "导出线程已经停止", "console.operations.export"));
        if (busy_ || job_.has_value())
            return Result<void>::failure(operations_error("IPC_BUSY", "已有导出任务正在执行",
                                                          "console.operations.export", true));
        busy_ = true;
        job_ = Job{.destination = std::move(destination),
                   .bytes = std::move(bytes),
                   .completion = std::move(completion)};
        condition_.notify_one();
        return Result<void>::success();
    }

    void stop() noexcept
    {
        {
            std::scoped_lock lock{mutex_};
            if (stopped_)
                return;
            stopped_ = true;
        }
        worker_.request_stop();
        condition_.notify_all();
        if (worker_.joinable())
            worker_.join();
    }

  private:
    struct Job final
    {
        std::filesystem::path destination;
        std::vector<std::byte> bytes;
        Completion completion;
    };

    static QString path_text(const std::filesystem::path& path)
    {
        const auto utf8 = path.generic_u8string();
        return QString::fromUtf8(reinterpret_cast<const char*>(utf8.data()),
                                 static_cast<qsizetype>(utf8.size()));
    }

    static Result<std::filesystem::path> write(const Job& job, const std::stop_token token)
    {
        QSaveFile file{path_text(job.destination)};
        if (!file.open(QIODevice::WriteOnly))
            return Result<std::filesystem::path>::failure(
                operations_error("SYS_DIAGNOSTIC_EXPORT_FAILED", "无法创建导出文件",
                                 "console.operations.export.open"));
        constexpr std::size_t chunk_size = 64U * 1024U;
        std::size_t offset = 0U;
        while (offset < job.bytes.size())
        {
            if (token.stop_requested())
            {
                file.cancelWriting();
                return Result<std::filesystem::path>::failure(operations_error(
                    "IPC_REQUEST_CANCELLED", "导出已取消", "console.operations.export.write"));
            }
            const std::size_t count = std::min(chunk_size, job.bytes.size() - offset);
            const auto* data = reinterpret_cast<const char*>(job.bytes.data() + offset);
            if (file.write(data, static_cast<qint64>(count)) != static_cast<qint64>(count))
            {
                file.cancelWriting();
                return Result<std::filesystem::path>::failure(
                    operations_error("SYS_DIAGNOSTIC_EXPORT_FAILED", "导出文件写入失败",
                                     "console.operations.export.write"));
            }
            offset += count;
        }
        if (!file.commit())
            return Result<std::filesystem::path>::failure(
                operations_error("SYS_DIAGNOSTIC_EXPORT_FAILED", "导出文件原子提交失败",
                                 "console.operations.export.commit"));
        return Result<std::filesystem::path>::success(job.destination);
    }

    void run(const std::stop_token token)
    {
        const auto thread_registration =
            register_thread_ ? register_thread_("console-diagnostics-export") : nullptr;
        while (!token.stop_requested())
        {
            std::optional<Job> job;
            {
                std::unique_lock lock{mutex_};
                condition_.wait(lock, token, [this] { return job_.has_value() || stopped_; });
                if (token.stop_requested() || stopped_)
                    return;
                job = std::move(job_);
                job_.reset();
            }
            auto result = write(*job, token);
            {
                std::scoped_lock lock{mutex_};
                busy_ = false;
            }
            if (job->completion)
            {
                auto completion = std::move(job->completion);
                QMetaObject::invokeMethod(
                    QCoreApplication::instance(),
                    [completion = std::move(completion), result = std::move(result)]() mutable {
                        completion(std::move(result));
                    },
                    Qt::QueuedConnection);
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable_any condition_;
    std::optional<Job> job_;
    bool busy_{};
    bool stopped_{};
    ThreadRegistrationFactory register_thread_;
    std::jthread worker_;
};

OperationsClient::OperationsClient(OperationsObserver observer, ipc::IpcClientOptions options,
                                   ThreadRegistrationFactory register_thread)
    : observer_(std::move(observer)),
      exporter_(std::make_unique<FileExporter>(std::move(register_thread))),
      alive_(std::make_shared<std::atomic_bool>(true))
{
    client_ = std::make_unique<ipc::IpcClient>(
        ipc::IpcClientCallbacks{
            .connection_changed = [this](const auto& value) { connection_changed(value); },
            .push_received = [this](const auto generation,
                                    const auto& push) { push_received(generation, push); }},
        std::move(options));
}

OperationsClient::~OperationsClient()
{
    alive_->store(false);
    stop();
}

Result<void> OperationsClient::start()
{
    return client_->start();
}

void OperationsClient::stop() noexcept
{
    if (client_)
        client_->stop();
    metrics_request_.reset();
    alarms_request_.reset();
    logs_request_.reset();
    operation_request_.reset();
    snapshot_.metrics_stale = true;
    snapshot_.alarms_stale = true;
    snapshot_.logs_stale = true;
    snapshot_.operation_pending = false;
    notify();
    if (exporter_)
        exporter_->stop();
}

const OperationsSnapshot& OperationsClient::snapshot() const noexcept
{
    return snapshot_;
}

void OperationsClient::connection_changed(const ipc::ClientConnectionSnapshot& connection)
{
    snapshot_.connection = connection;
    if (connection.state != ipc::ClientConnectionState::connected)
    {
        snapshot_.metrics_stale = true;
        snapshot_.alarms_stale = true;
        snapshot_.logs_stale = true;
        metrics_request_.reset();
        alarms_request_.reset();
        logs_request_.reset();
        operation_request_.reset();
        snapshot_.operation_pending = false;
    }
    else
        refresh();
    notify();
}

void OperationsClient::push_received(const std::uint64_t generation, const ipc::PushMessage& push)
{
    if (generation == snapshot_.connection.generation &&
        (push.event_name == "alarm.raised" || push.event_name == "alarm.cleared" ||
         push.event_name == "alarm.acknowledged"))
        refresh_alarms();
}

void OperationsClient::refresh()
{
    refresh_metrics();
    refresh_alarms();
    refresh_logs();
}

void OperationsClient::refresh_metrics()
{
    if (metrics_request_ || snapshot_.connection.state != ipc::ClientConnectionState::connected)
        return;
    auto sent = client_->send_request("system.getMetrics", R"({"limit":256})", {},
                                      [this](auto handle, auto result) {
                                          metrics_completed(std::move(handle), std::move(result));
                                      });
    if (!sent)
    {
        snapshot_.error = sent.error();
        notify();
        return;
    }
    metrics_request_ = std::move(sent).value();
}

void OperationsClient::refresh_alarms()
{
    if (alarms_request_ || snapshot_.connection.state != ipc::ClientConnectionState::connected)
        return;
    Json payload{{"limit", 200}};
    if (snapshot_.alarm_filter.active)
        payload["active"] = *snapshot_.alarm_filter.active;
    if (snapshot_.alarm_filter.minimum_severity)
        payload["minimumSeverity"] = *snapshot_.alarm_filter.minimum_severity;
    if (snapshot_.alarm_filter.source && !snapshot_.alarm_filter.source->empty())
        payload["source"] = *snapshot_.alarm_filter.source;
    auto sent =
        client_->send_request("alarm.list", payload.dump(), {}, [this](auto handle, auto result) {
            alarms_completed(std::move(handle), std::move(result));
        });
    if (!sent)
    {
        snapshot_.error = sent.error();
        notify();
        return;
    }
    alarms_request_ = std::move(sent).value();
}

void OperationsClient::refresh_logs()
{
    if (logs_request_ || snapshot_.connection.state != ipc::ClientConnectionState::connected)
        return;
    Json payload{{"limit", 200}};
    if (snapshot_.log_filter.category && !snapshot_.log_filter.category->empty())
        payload["categories"] = Json::array({*snapshot_.log_filter.category});
    if (snapshot_.log_filter.minimum_level)
        payload["minimumLevel"] = *snapshot_.log_filter.minimum_level;
    if (snapshot_.log_filter.thread_name && !snapshot_.log_filter.thread_name->empty())
        payload["threadName"] = *snapshot_.log_filter.thread_name;
    auto sent =
        client_->send_request("log.tail", payload.dump(), {}, [this](auto handle, auto result) {
            logs_completed(std::move(handle), std::move(result));
        });
    if (!sent)
    {
        snapshot_.error = sent.error();
        notify();
        return;
    }
    logs_request_ = std::move(sent).value();
}

Result<void> OperationsClient::query_alarms(AlarmFilter filter)
{
    if (snapshot_.connection.state != ipc::ClientConnectionState::connected)
        return Result<void>::failure(operations_error("IPC_NOT_CONNECTED", "后台服务未连接",
                                                      "console.operations.alarms", true));
    if (alarms_request_)
        return Result<void>::failure(
            operations_error("IPC_BUSY", "报警查询正在执行", "console.operations.alarms", true));
    snapshot_.alarm_filter = std::move(filter);
    refresh_alarms();
    return Result<void>::success();
}

Result<void> OperationsClient::query_logs(LogFilter filter)
{
    if (snapshot_.connection.state != ipc::ClientConnectionState::connected)
        return Result<void>::failure(operations_error("IPC_NOT_CONNECTED", "后台服务未连接",
                                                      "console.operations.logs", true));
    if (logs_request_)
        return Result<void>::failure(
            operations_error("IPC_BUSY", "日志查询正在执行", "console.operations.logs", true));
    snapshot_.log_filter = std::move(filter);
    refresh_logs();
    return Result<void>::success();
}

void OperationsClient::metrics_completed(ipc::ClientRequestHandle handle,
                                         Result<ipc::ResponseMessage> result)
{
    if (!metrics_request_ || *metrics_request_ != handle ||
        handle.generation != snapshot_.connection.generation)
        return;
    metrics_request_.reset();
    auto payload = response_payload(result, "console.operations.metrics");
    if (!payload || !payload.value().contains("metrics") ||
        !payload.value()["metrics"].is_array() || payload.value()["metrics"].size() > 256U)
    {
        snapshot_.error = payload ? operations_error("IPC_PROTOCOL_ERROR", "指标响应结构无效",
                                                     "console.operations.metrics")
                                  : payload.error();
        snapshot_.metrics_stale = true;
        notify();
        return;
    }
    std::vector<OperationsMetric> items;
    for (const auto& item : payload.value()["metrics"])
    {
        if (!item.is_object() || !item.contains("name") || !item["name"].is_string() ||
            !item.contains("value") || !item.contains("unit") || !item["unit"].is_string() ||
            !item.contains("available") || !item["available"].is_boolean())
        {
            snapshot_.error = operations_error("IPC_PROTOCOL_ERROR", "指标项结构无效",
                                               "console.operations.metrics");
            snapshot_.metrics_stale = true;
            notify();
            return;
        }
        items.push_back({.name = item["name"].get<std::string>(),
                         .value = json_value_text(item["value"]),
                         .unit = item["unit"].get<std::string>(),
                         .available = item["available"].get<bool>()});
    }
    snapshot_.metrics = std::move(items);
    snapshot_.metrics_stale = false;
    snapshot_.error.reset();
    notify();
}

void OperationsClient::alarms_completed(ipc::ClientRequestHandle handle,
                                        Result<ipc::ResponseMessage> result)
{
    if (!alarms_request_ || *alarms_request_ != handle ||
        handle.generation != snapshot_.connection.generation)
        return;
    alarms_request_.reset();
    auto payload = response_payload(result, "console.operations.alarms");
    if (!payload || !payload.value().contains("alarms") || !payload.value()["alarms"].is_array() ||
        payload.value()["alarms"].size() > 200U)
    {
        snapshot_.error = payload ? operations_error("IPC_PROTOCOL_ERROR", "报警响应结构无效",
                                                     "console.operations.alarms")
                                  : payload.error();
        snapshot_.alarms_stale = true;
        notify();
        return;
    }
    std::vector<OperationsAlarm> items;
    for (const auto& item : payload.value()["alarms"])
    {
        if (!item.is_object() || !item.contains("alarmId") ||
            !item["alarmId"].is_number_unsigned() || !item.contains("code") ||
            !item["code"].is_string() || !item.contains("severity") ||
            !item["severity"].is_string() || !item.contains("source") ||
            !item["source"].is_string() || !item.contains("firstOccurredAt") ||
            !item["firstOccurredAt"].is_string() || !item.contains("lastOccurredAt") ||
            !item["lastOccurredAt"].is_string() || !item.contains("active") ||
            !item["active"].is_boolean() || !item.contains("occurrenceCount") ||
            !item["occurrenceCount"].is_number_unsigned() || !item.contains("message") ||
            !item["message"].is_string() || !item.contains("acknowledged") ||
            !item["acknowledged"].is_boolean())
        {
            snapshot_.error = operations_error("IPC_PROTOCOL_ERROR", "报警项结构无效",
                                               "console.operations.alarms");
            snapshot_.alarms_stale = true;
            notify();
            return;
        }
        OperationsAlarm alarm{.alarm_id = item["alarmId"].get<std::uint64_t>(),
                              .code = item["code"].get<std::string>(),
                              .severity = item["severity"].get<std::string>(),
                              .source = item["source"].get<std::string>(),
                              .first_occurred_at = item["firstOccurredAt"].get<std::string>(),
                              .last_occurred_at = item["lastOccurredAt"].get<std::string>(),
                              .active = item["active"].get<bool>(),
                              .occurrence_count = item["occurrenceCount"].get<std::uint64_t>(),
                              .message = item["message"].get<std::string>(),
                              .acknowledged = item["acknowledged"].get<bool>()};
        if (item.contains("details") && item["details"].is_object())
            for (const auto& [key, value] : item["details"].items())
                alarm.details.emplace_back(key, json_value_text(value));
        items.push_back(std::move(alarm));
    }
    snapshot_.alarms = std::move(items);
    snapshot_.alarms_truncated = payload.value().value("truncated", false);
    snapshot_.alarms_stale = false;
    snapshot_.error.reset();
    notify();
}

void OperationsClient::logs_completed(ipc::ClientRequestHandle handle,
                                      Result<ipc::ResponseMessage> result)
{
    if (!logs_request_ || *logs_request_ != handle ||
        handle.generation != snapshot_.connection.generation)
        return;
    logs_request_.reset();
    auto payload = response_payload(result, "console.operations.logs");
    if (!payload || !payload.value().contains("records") ||
        !payload.value()["records"].is_array() || payload.value()["records"].size() > 200U)
    {
        snapshot_.error = payload ? operations_error("IPC_PROTOCOL_ERROR", "日志响应结构无效",
                                                     "console.operations.logs")
                                  : payload.error();
        snapshot_.logs_stale = true;
        notify();
        return;
    }
    std::vector<OperationsLogRecord> items;
    for (const auto& item : payload.value()["records"])
    {
        if (!item.is_object() || !item.contains("sequence") ||
            !item["sequence"].is_number_unsigned() || !item.contains("timestamp") ||
            !item["timestamp"].is_string() || !item.contains("threadId") ||
            !item["threadId"].is_number_unsigned() || !item.contains("category") ||
            !item["category"].is_string() || !item.contains("level") ||
            !item["level"].is_string() || !item.contains("message") || !item["message"].is_string())
        {
            snapshot_.error =
                operations_error("IPC_PROTOCOL_ERROR", "日志项结构无效", "console.operations.logs");
            snapshot_.logs_stale = true;
            notify();
            return;
        }
        items.push_back(
            {.sequence = item["sequence"].get<std::uint64_t>(),
             .timestamp = item["timestamp"].get<std::string>(),
             .thread_id = item["threadId"].get<std::uint64_t>(),
             .thread_name = item.value("threadName",
                                       std::string{"unregistered-thread-"} +
                                           std::to_string(item["threadId"].get<std::uint64_t>())),
             .category = item["category"].get<std::string>(),
             .level = item["level"].get<std::string>(),
             .message = item["message"].get<std::string>()});
    }
    snapshot_.logs = std::move(items);
    snapshot_.logs_truncated = payload.value().value("truncated", false);
    snapshot_.logs_stale = false;
    snapshot_.error.reset();
    notify();
}

Result<void> OperationsClient::acknowledge(const std::uint64_t alarm_id)
{
    if (operation_request_ || snapshot_.operation_pending)
        return Result<void>::failure(operations_error("IPC_BUSY", "已有运维操作正在执行",
                                                      "console.operations.acknowledge", true));
    if (snapshot_.connection.state != ipc::ClientConnectionState::connected)
        return Result<void>::failure(operations_error("IPC_NOT_CONNECTED", "后台服务未连接",
                                                      "console.operations.acknowledge", true));
    snapshot_.operation_pending = true;
    snapshot_.operation = "alarm.acknowledge";
    auto sent =
        client_->send_request("alarm.acknowledge", Json{{"alarmId", alarm_id}}.dump(), {},
                              [this](auto handle, auto result) {
                                  operation_completed(std::move(handle), std::move(result), {});
                              });
    if (!sent)
    {
        snapshot_.operation_pending = false;
        snapshot_.error = sent.error();
        notify();
        return Result<void>::failure(sent.error());
    }
    operation_request_ = std::move(sent).value();
    notify();
    return Result<void>::success();
}

Result<void> OperationsClient::export_diagnostics(std::filesystem::path destination)
{
    if (destination.empty())
        return Result<void>::failure(operations_error("IPC_REQUEST_INVALID", "导出目标路径为空",
                                                      "console.operations.export"));
    if (operation_request_ || snapshot_.operation_pending)
        return Result<void>::failure(operations_error("IPC_BUSY", "已有运维操作正在执行",
                                                      "console.operations.export", true));
    if (snapshot_.connection.state != ipc::ClientConnectionState::connected)
        return Result<void>::failure(operations_error("IPC_NOT_CONNECTED", "后台服务未连接",
                                                      "console.operations.export", true));
    snapshot_.operation_pending = true;
    snapshot_.operation = "system.exportDiagnostics";
    auto sent = client_->send_request(
        "system.exportDiagnostics", "{}", {},
        [this, destination](auto handle, auto result) mutable {
            operation_completed(std::move(handle), std::move(result), std::move(destination));
        },
        std::chrono::seconds{15});
    if (!sent)
    {
        snapshot_.operation_pending = false;
        snapshot_.error = sent.error();
        notify();
        return Result<void>::failure(sent.error());
    }
    operation_request_ = std::move(sent).value();
    notify();
    return Result<void>::success();
}

Result<void> OperationsClient::export_alarm_csv(std::filesystem::path destination)
{
    if (destination.empty())
        return Result<void>::failure(operations_error("IPC_REQUEST_INVALID", "导出目标路径为空",
                                                      "console.operations.alarmExport"));
    if (snapshot_.alarms_stale)
        return Result<void>::failure(operations_error("IPC_NOT_CONNECTED",
                                                      "报警数据不可用或已过期，不能导出",
                                                      "console.operations.alarmExport", true));
    std::string csv = "alarmId,code,severity,source,firstOccurredAt,lastOccurredAt,active,"
                      "occurrenceCount,acknowledged,message\r\n";
    for (const auto& alarm : snapshot_.alarms)
    {
        csv += std::to_string(alarm.alarm_id) + ',' + csv_field(alarm.code) + ',' +
               csv_field(alarm.severity) + ',' + csv_field(alarm.source) + ',' +
               csv_field(alarm.first_occurred_at) + ',' + csv_field(alarm.last_occurred_at) + ',' +
               (alarm.active ? "true" : "false") + ',' + std::to_string(alarm.occurrence_count) +
               ',' + (alarm.acknowledged ? "true" : "false") + ',' + csv_field(alarm.message) +
               "\r\n";
    }
    std::vector<std::byte> bytes;
    bytes.reserve(csv.size() + 3U);
    bytes.insert(bytes.end(), {std::byte{0xef}, std::byte{0xbb}, std::byte{0xbf}});
    for (const unsigned char character : csv)
        bytes.push_back(static_cast<std::byte>(character));
    snapshot_.operation_pending = true;
    snapshot_.operation = "alarm.exportCsv";
    const auto alive = alive_;
    auto submitted = exporter_->submit(std::move(destination), std::move(bytes),
                                       [this, alive](Result<std::filesystem::path> result) mutable {
                                           if (alive->load())
                                               export_completed(std::move(result));
                                       });
    if (!submitted)
    {
        snapshot_.operation_pending = false;
        snapshot_.error = submitted.error();
        notify();
        return submitted;
    }
    notify();
    return Result<void>::success();
}

void OperationsClient::operation_completed(ipc::ClientRequestHandle handle,
                                           Result<ipc::ResponseMessage> result,
                                           std::filesystem::path destination)
{
    if (!operation_request_ || *operation_request_ != handle ||
        handle.generation != snapshot_.connection.generation)
        return;
    operation_request_.reset();
    if (snapshot_.operation == "alarm.acknowledge")
    {
        auto payload = response_payload(result, "console.operations.acknowledge");
        snapshot_.operation_pending = false;
        if (!payload)
            snapshot_.error = payload.error();
        else
        {
            snapshot_.error.reset();
            refresh_alarms();
        }
        notify();
        return;
    }

    auto payload = response_payload(result, "console.operations.export");
    if (!payload || result.value().binary.empty() || !payload.value().contains("size") ||
        !payload.value()["size"].is_number_unsigned() ||
        payload.value()["size"].get<std::uint64_t>() != result.value().binary.size() ||
        payload.value().value("contentType", "") != "application/zip" ||
        !payload.value().value("redacted", false))
    {
        snapshot_.operation_pending = false;
        snapshot_.error = payload
                              ? operations_error("IPC_PROTOCOL_ERROR", "诊断包响应结构或长度无效",
                                                 "console.operations.export")
                              : payload.error();
        notify();
        return;
    }
    const auto alive = alive_;
    auto submitted =
        exporter_->submit(std::move(destination), std::move(result.value().binary),
                          [this, alive](Result<std::filesystem::path> export_result) mutable {
                              if (alive->load())
                                  export_completed(std::move(export_result));
                          });
    if (!submitted)
    {
        snapshot_.operation_pending = false;
        snapshot_.error = submitted.error();
        notify();
    }
}

void OperationsClient::export_completed(Result<std::filesystem::path> result)
{
    snapshot_.operation_pending = false;
    if (result)
    {
        snapshot_.exported_path = std::move(result).value();
        snapshot_.error.reset();
    }
    else
        snapshot_.error = result.error();
    notify();
}

void OperationsClient::notify() const noexcept
{
    try
    {
        if (observer_)
            observer_(snapshot_);
    }
    catch (...)
    {
    }
}

} // namespace paperbreak::console
