#include "paperbreak/uplink/qt_transport.hpp"

#include "paperbreak/common/error.hpp"

#include <QCryptographicHash>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string_view>
#include <utility>

namespace paperbreak::uplink
{
namespace
{
using Json = nlohmann::json;

struct HttpResponse final
{
    int status{};
    QByteArray body;
};

Error transport_error(std::string code, std::string message, std::string operation,
                      const bool retryable = false)
{
    return make_error(std::move(code), retryable ? Severity::warning : Severity::error,
                      std::move(message), "uplink-transport", std::move(operation), retryable);
}

Error protocol_error(std::string message, std::string operation)
{
    return transport_error("UPLINK_PROTOCOL_ERROR", std::move(message), std::move(operation));
}

std::string utf8_path(const std::filesystem::path& path)
{
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

QString qstring_path(const std::string_view path)
{
    return QString::fromUtf8(path.data(), static_cast<qsizetype>(path.size()));
}

Result<std::string> sha256_file(QFile& file, const std::stop_token stop_token,
                                const std::uint64_t expected_bytes,
                                const std::uint64_t limit_bytes_per_second)
{
    if (!file.seek(0))
        return Result<std::string>::failure(transport_error(
            "UPLOAD_TRANSFER_FAILED", "无法定位上传源文件", "uplink.upload.hash.seek", true));
    QCryptographicHash hash{QCryptographicHash::Sha256};
    std::uint64_t read_bytes = 0U;
    const auto started_at = std::chrono::steady_clock::now();
    while (!file.atEnd())
    {
        if (stop_token.stop_requested())
            return Result<std::string>::failure(transport_error(
                "UPLOAD_TRANSFER_INTERRUPTED", "上传校验已取消", "uplink.upload.hash", true));
        const QByteArray block = file.read(1024 * 1024);
        if (block.isEmpty() && file.error() != QFileDevice::NoError)
            return Result<std::string>::failure(transport_error(
                "UPLOAD_TRANSFER_FAILED", "读取上传源文件失败", "uplink.upload.hash.read", true));
        read_bytes += static_cast<std::uint64_t>(block.size());
        hash.addData(block);
        const auto expected_elapsed =
            std::chrono::duration<long double>{static_cast<long double>(read_bytes) /
                                               static_cast<long double>(limit_bytes_per_second)};
        while (std::chrono::steady_clock::now() - started_at < expected_elapsed)
        {
            if (stop_token.stop_requested())
                return Result<std::string>::failure(
                    transport_error("UPLOAD_TRANSFER_INTERRUPTED", "上传校验限速等待已取消",
                                    "uplink.upload.hash.throttle", true));
            QThread::msleep(1U);
        }
    }
    if (read_bytes != expected_bytes)
        return Result<std::string>::failure(transport_error("UPLOAD_SOURCE_CHANGED",
                                                            "上传源文件长度在任务创建后发生变化",
                                                            "uplink.upload.hash.length"));
    return Result<std::string>::success(hash.result().toHex().toStdString());
}

std::optional<Json> parse_object(const QByteArray& body)
{
    if (body.size() <= 0 || body.size() > static_cast<qsizetype>(maximum_json_message_bytes))
        return std::nullopt;
    auto value = Json::parse(body.constData(), body.constData() + body.size(), nullptr, false);
    if (value.is_discarded() || !value.is_object())
        return std::nullopt;
    return value;
}

Error response_error(const HttpResponse& response, const std::string_view operation)
{
    std::string code;
    std::string message;
    bool retryable = response.status == 408 || response.status == 425 || response.status == 429 ||
                     response.status == 507 || response.status >= 500;
    if (auto parsed = parse_object(response.body);
        parsed && parsed->contains("error") && parsed->at("error").is_object())
    {
        const auto& error = parsed->at("error");
        if (error.contains("businessCode") && error.at("businessCode").is_string())
            code = error.at("businessCode").get<std::string>();
        if (error.contains("message") && error.at("message").is_string())
            message = error.at("message").get<std::string>();
        if (error.contains("retryable") && error.at("retryable").is_boolean())
            retryable = error.at("retryable").get<bool>();
    }
    if (code.empty())
        code = retryable ? "UPLOAD_TRANSFER_FAILED" : "UPLOAD_REJECTED";
    if (message.empty())
        message = "上位机 HTTP 请求失败，状态码 " + std::to_string(response.status);
    auto error =
        transport_error(std::move(code), std::move(message), std::string{operation}, retryable);
    error.native_domain = "HTTP";
    error.native_code = std::to_string(response.status);
    return error;
}

std::string upload_checkpoint(const std::string_view upload_id,
                              const std::set<std::uint32_t>& chunks, const std::string_view sha256)
{
    constexpr std::size_t maximum_checkpoint_chunk_indices = 1024U;
    Json checkpoint{
        {"uploadId", upload_id}, {"receivedChunkCount", chunks.size()}, {"sha256", sha256}};
    Json indices = Json::array();
    for (const auto index : chunks | std::views::take(maximum_checkpoint_chunk_indices))
        indices.push_back(index);
    checkpoint["receivedChunks"] = std::move(indices);
    checkpoint["receivedChunksTruncated"] = chunks.size() > maximum_checkpoint_chunk_indices;
    return checkpoint.dump();
}

Error with_checkpoint(Error error, const std::string_view checkpoint)
{
    if (checkpoint.size() <= maximum_json_message_bytes)
        error.details.push_back({"uploadCheckpoint", std::string{checkpoint}});
    return error;
}

std::string checkpoint_from_error(const Error& error, const std::string_view fallback)
{
    const auto found = std::ranges::find_if(
        error.details, [](const ErrorDetail& detail) { return detail.key == "uploadCheckpoint"; });
    return found == error.details.end() ? std::string{fallback} : found->value;
}

std::string content_type(const storage::UploadJobKind kind, const std::filesystem::path& path)
{
    if (kind == storage::UploadJobKind::manifest || path.extension() == ".json")
        return "application/json";
    if (path.extension() == ".jpg" || path.extension() == ".jpeg")
        return "image/jpeg";
    if (path.extension() == ".mp4")
        return "video/mp4";
    return "application/octet-stream";
}

bool relative_path_is_safe(const std::filesystem::path& path)
{
    if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory())
        return false;
    return std::ranges::none_of(path, [](const auto& component) { return component == ".."; });
}

class WebSocketWorker final : public QObject
{
  public:
    explicit WebSocketWorker(std::function<void(const MessageEnvelope&)> dispatch)
        : dispatch_(std::move(dispatch))
    {
    }

    Result<void> connect_to(const QUrl& url, const std::chrono::milliseconds timeout)
    {
        shutdown();
        socket_ = std::make_unique<QWebSocket>();
        socket_->setMaxAllowedIncomingMessageSize(maximum_json_message_bytes);
        socket_->setMaxAllowedIncomingFrameSize(maximum_json_message_bytes);
        connected_ = false;
        failed_ = false;
        QObject::connect(socket_.get(), &QWebSocket::connected, this, [this] {
            connected_ = true;
            if (active_loop_ != nullptr)
                active_loop_->quit();
        });
        QObject::connect(socket_.get(), &QWebSocket::disconnected, this, [this] {
            connected_ = false;
            failed_ = true;
            if (active_loop_ != nullptr)
                active_loop_->quit();
        });
        QObject::connect(socket_.get(), &QWebSocket::errorOccurred, this,
                         [this](QAbstractSocket::SocketError) {
                             failed_ = true;
                             if (active_loop_ != nullptr)
                                 active_loop_->quit();
                         });
        QObject::connect(socket_.get(), &QWebSocket::textMessageReceived, this,
                         [this](const QString& text) { receive(text); });
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        active_loop_ = &loop;
        timer.start(static_cast<int>(timeout.count()));
        socket_->open(url);
        loop.exec();
        active_loop_ = nullptr;
        if (!connected_)
        {
            shutdown();
            return Result<void>::failure(transport_error(
                failed_ ? "UPLINK_DISCONNECTED" : "UPLINK_TIMEOUT",
                failed_ ? "无法建立上位机 WebSocket 会话" : "建立 WebSocket 会话超时",
                "uplink.websocket.connect", true));
        }
        return Result<void>::success();
    }

    Result<TransportAcknowledgement> send(const MessageEnvelope& message,
                                          const std::chrono::milliseconds timeout)
    {
        if (!socket_ || socket_->state() != QAbstractSocket::ConnectedState)
            return Result<TransportAcknowledgement>::failure(transport_error(
                "UPLINK_DISCONNECTED", "WebSocket 会话未连接", "uplink.websocket.send", true));
        auto encoded = serialize_message_envelope(message);
        if (!encoded)
            return Result<TransportAcknowledgement>::failure(encoded.error());
        pending_message_id_ = message.message_id;
        acknowledgement_.reset();
        failed_ = false;
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        active_loop_ = &loop;
        timer.start(static_cast<int>(timeout.count()));
        if (socket_->sendTextMessage(QString::fromStdString(encoded.value())) < 0)
        {
            active_loop_ = nullptr;
            return Result<TransportAcknowledgement>::failure(transport_error(
                "UPLINK_DISCONNECTED", "无法发送 WebSocket 消息", "uplink.websocket.send", true));
        }
        loop.exec();
        active_loop_ = nullptr;
        pending_message_id_.clear();
        if (acknowledgement_)
            return Result<TransportAcknowledgement>::success(*acknowledgement_);
        return Result<TransportAcknowledgement>::failure(
            transport_error(failed_ ? "UPLINK_DISCONNECTED" : "UPLINK_TIMEOUT",
                            failed_ ? "等待 WebSocket 确认时连接断开" : "等待 WebSocket 确认超时",
                            "uplink.websocket.ack", true));
    }

    void cancel() noexcept
    {
        failed_ = true;
        connected_ = false;
        if (socket_)
            socket_->abort();
        if (active_loop_ != nullptr)
            active_loop_->quit();
    }

    void shutdown() noexcept
    {
        cancel();
        socket_.reset();
        pending_message_id_.clear();
        acknowledgement_.reset();
    }

  private:
    void receive(const QString& text)
    {
        auto envelope = parse_message_envelope(text.toStdString());
        if (!envelope)
        {
            failed_ = true;
            if (active_loop_ != nullptr)
                active_loop_->quit();
            return;
        }
        if (envelope.value().message_type == "command")
        {
            dispatch_(envelope.value());
            return;
        }
        if (envelope.value().message_type != "ack" || pending_message_id_.empty())
            return;
        auto payload = Json::parse(envelope.value().payload_json, nullptr, false);
        if (!payload.is_object() || !payload.contains("acknowledgedMessageId") ||
            !payload.at("acknowledgedMessageId").is_string() ||
            payload.at("acknowledgedMessageId").get<std::string>() != pending_message_id_)
            return;
        acknowledgement_ = TransportAcknowledgement{.correlation_id = pending_message_id_,
                                                    .acknowledged_at = envelope.value().timestamp};
        if (active_loop_ != nullptr)
            active_loop_->quit();
    }

    std::function<void(const MessageEnvelope&)> dispatch_;
    std::unique_ptr<QWebSocket> socket_;
    QEventLoop* active_loop_{};
    std::string pending_message_id_;
    std::optional<TransportAcknowledgement> acknowledgement_;
    bool connected_{};
    bool failed_{};
};

} // namespace

struct QtUplinkTransport::Impl final
{
    explicit Impl(QtUplinkTransportConfig value)
        : config(std::move(value)), base_url(QString::fromStdString(config.server_url)),
          websocket_worker(std::make_unique<WebSocketWorker>(
              [this](const MessageEnvelope& command) { dispatch_command(command); }))
    {
        websocket_worker->moveToThread(&websocket_thread);
        QObject::connect(&websocket_thread, &QThread::started, [this] {
            websocket_thread_registration =
                config.register_thread ? config.register_thread("uplink-transport") : nullptr;
        });
        QObject::connect(&websocket_thread, &QThread::finished,
                         [this] { websocket_thread_registration.reset(); });
        websocket_thread.start();
    }

    ~Impl()
    {
        cancel();
        if (websocket_thread.isRunning())
        {
            QMetaObject::invokeMethod(
                websocket_worker.get(),
                [this] {
                    websocket_worker->shutdown();
                    websocket_worker->moveToThread(nullptr);
                },
                Qt::BlockingQueuedConnection);
            websocket_thread.quit();
            websocket_thread.wait(static_cast<unsigned long>(config.io_timeout.count()));
        }
    }

    QtUplinkTransportConfig config;
    QUrl base_url;
    std::atomic<UplinkConnectionState> state{UplinkConnectionState::disconnected};
    std::atomic_uint64_t cancel_generation{};
    mutable std::mutex handler_mutex;
    CommandHandler command_handler;
    mutable std::mutex websocket_call_mutex;
    QThread websocket_thread;
    std::shared_ptr<void> websocket_thread_registration;
    std::unique_ptr<WebSocketWorker> websocket_worker;
    mutable std::mutex http_call_mutex;
    mutable std::mutex reply_mutex;
    QPointer<QNetworkReply> current_reply;

    void dispatch_command(const MessageEnvelope& command)
    {
        CommandHandler handler;
        {
            std::scoped_lock lock{handler_mutex};
            handler = command_handler;
        }
        if (handler)
            handler(command);
    }

    QUrl endpoint(const QString& path) const
    {
        QUrl result = base_url;
        result.setPath(path);
        result.setQuery({});
        result.setFragment({});
        return result;
    }

    Result<HttpResponse> http(const QByteArray& method, const QUrl& url, const QByteArray& body,
                              const std::vector<std::pair<QByteArray, QByteArray>>& headers = {},
                              const std::stop_token stop_token = {})
    {
        std::scoped_lock http_lock{http_call_mutex};
        const auto generation = cancel_generation.load(std::memory_order_acquire);
        if (stop_token.stop_requested())
            return Result<HttpResponse>::failure(transport_error(
                "UPLOAD_TRANSFER_INTERRUPTED", "HTTP 请求已取消", "uplink.http", true));
        QNetworkAccessManager manager;
        QNetworkRequest request{url};
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        for (const auto& [name, value] : headers)
            request.setRawHeader(name, value);
        QNetworkReply* reply = nullptr;
        if (method == "GET")
            reply = manager.get(request);
        else if (method == "POST")
            reply = manager.post(request, body);
        else if (method == "PUT")
            reply = manager.put(request, body);
        if (reply == nullptr)
            return Result<HttpResponse>::failure(transport_error(
                "UPLOAD_TRANSFER_FAILED", "无法创建 HTTP 请求", "uplink.http", true));
        {
            std::scoped_lock lock{reply_mutex};
            current_reply = reply;
        }
        QEventLoop loop;
        QTimer timeout;
        QTimer cancellation;
        QByteArray response;
        bool timed_out = false;
        bool response_too_large = false;
        timeout.setSingleShot(true);
        cancellation.setInterval(10);
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QObject::connect(reply, &QIODevice::readyRead, reply, [&] {
            response += reply->readAll();
            if (response.size() > static_cast<qsizetype>(maximum_json_message_bytes))
            {
                response_too_large = true;
                reply->abort();
            }
        });
        QObject::connect(&timeout, &QTimer::timeout, reply, [&] {
            timed_out = true;
            reply->abort();
        });
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(&cancellation, &QTimer::timeout, reply,
                         [this, reply, generation, stop_token] {
                             if (stop_token.stop_requested() ||
                                 cancel_generation.load(std::memory_order_acquire) != generation)
                                 reply->abort();
                         });
        timeout.start(static_cast<int>(config.io_timeout.count()));
        cancellation.start();
        loop.exec();
        cancellation.stop();
        response += reply->readAll();
        const bool cancelled = stop_token.stop_requested() ||
                               cancel_generation.load(std::memory_order_acquire) != generation;
        if (!reply->isFinished())
            reply->abort();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        {
            std::scoped_lock lock{reply_mutex};
            if (current_reply == reply)
                current_reply.clear();
        }
        reply->deleteLater();
        if (cancelled)
            return Result<HttpResponse>::failure(transport_error(
                "UPLOAD_TRANSFER_INTERRUPTED", "HTTP 请求已取消", "uplink.http", true));
        if (timed_out)
            return Result<HttpResponse>::failure(
                transport_error("UPLINK_TIMEOUT", "HTTP 请求超时", "uplink.http", true));
        if (response_too_large ||
            response.size() > static_cast<qsizetype>(maximum_json_message_bytes))
            return Result<HttpResponse>::failure(
                protocol_error("HTTP 响应超过 1 MiB", "uplink.http.response"));
        if (status == 0)
            return Result<HttpResponse>::failure(
                transport_error("UPLINK_DISCONNECTED", "HTTP 连接失败", "uplink.http", true));
        return Result<HttpResponse>::success({.status = status, .body = response});
    }

    Result<TransportAcknowledgement> websocket_send(const MessageEnvelope& message)
    {
        if (state.load(std::memory_order_acquire) != UplinkConnectionState::connected)
            return Result<TransportAcknowledgement>::failure(transport_error(
                "UPLINK_DISCONNECTED", "上位机会话未连接", "uplink.websocket.send", true));
        std::scoped_lock lock{websocket_call_mutex};
        Result<TransportAcknowledgement> result =
            Result<TransportAcknowledgement>::failure(transport_error(
                "UPLINK_DISCONNECTED", "WebSocket 工作线程不可用", "uplink.websocket.send", true));
        QMetaObject::invokeMethod(
            websocket_worker.get(),
            [this, &result, &message] {
                result = websocket_worker->send(message, config.io_timeout);
            },
            Qt::BlockingQueuedConnection);
        if (!result)
            state.store(UplinkConnectionState::disconnected, std::memory_order_release);
        return result;
    }

    void cancel() noexcept
    {
        state.store(UplinkConnectionState::disconnected, std::memory_order_release);
        cancel_generation.fetch_add(1U, std::memory_order_acq_rel);
        QPointer<QNetworkReply> reply;
        {
            std::scoped_lock lock{reply_mutex};
            reply = current_reply;
        }
        if (reply)
            QMetaObject::invokeMethod(reply, &QNetworkReply::abort, Qt::QueuedConnection);
        if (websocket_thread.isRunning() && websocket_worker)
            QMetaObject::invokeMethod(
                websocket_worker.get(), [this] { websocket_worker->cancel(); },
                Qt::QueuedConnection);
    }
};

QtUplinkTransport::QtUplinkTransport(ValidatedTag, std::shared_ptr<Impl> impl)
    : impl_(std::move(impl))
{
}

QtUplinkTransport::~QtUplinkTransport() = default;

Result<std::unique_ptr<QtUplinkTransport>> QtUplinkTransport::create(QtUplinkTransportConfig config)
{
    const QUrl url{QString::fromStdString(config.server_url)};
    if (!url.isValid() || url.scheme() != "http" || url.host().isEmpty() ||
        config.io_timeout < std::chrono::milliseconds{100} ||
        config.io_timeout > std::chrono::seconds{60} || config.chunk_bytes < 64U * 1024U ||
        config.chunk_bytes > maximum_chunk_bytes || config.upload_limit_bytes_per_second == 0U ||
        config.upload_limit_bytes_per_second > maximum_upload_limit_bytes_per_second)
        return Result<std::unique_ptr<QtUplinkTransport>>::failure(protocol_error(
            "Uplink v1 基址、I/O 超时、分块或上传速率上限无效", "uplink.transport.create"));
    while (config.server_url.ends_with('/'))
        config.server_url.pop_back();
    return Result<std::unique_ptr<QtUplinkTransport>>::success(std::make_unique<QtUplinkTransport>(
        ValidatedTag{}, std::make_shared<Impl>(std::move(config))));
}

Result<TransportSession> QtUplinkTransport::connect(const SessionHello& hello)
{
    if (auto valid = validate_identifier(hello.request_id, "requestId", 128U); !valid)
        return Result<TransportSession>::failure(valid.error());
    if (auto valid = validate_identifier(hello.machine_id, "machineId", 64U); !valid)
        return Result<TransportSession>::failure(valid.error());
    impl_->cancel();
    const QByteArray body = QByteArray::fromStdString(Json{
        {"requestId", hello.request_id},
        {"machineId", hello.machine_id},
        {"productionLineId", hello.production_line_id},
        {"softwareVersion", hello.software_version},
        {"supportedProtocolVersions", hello.supported_protocol_versions},
        {"capabilities", hello.capabilities}}.dump());
    auto response = impl_->http("POST", impl_->endpoint("/api/uplink/v1/sessions"), body);
    if (!response)
        return Result<TransportSession>::failure(response.error());
    if (response.value().status != 200 && response.value().status != 201)
        return Result<TransportSession>::failure(
            response_error(response.value(), "uplink.session.create"));
    auto parsed = parse_object(response.value().body);
    if (!parsed || !parsed->contains("sessionId") || !parsed->at("sessionId").is_string() ||
        !parsed->contains("machineId") || !parsed->at("machineId").is_string() ||
        !parsed->contains("protocolVersion") ||
        !parsed->at("protocolVersion").is_number_unsigned() ||
        !parsed->contains("heartbeatSeconds") ||
        !parsed->at("heartbeatSeconds").is_number_unsigned() || !parsed->contains("webSocketUrl") ||
        !parsed->at("webSocketUrl").is_string())
        return Result<TransportSession>::failure(
            protocol_error("会话响应字段不完整", "uplink.session.response"));
    const auto machine_id = parsed->at("machineId").get<std::string>();
    const auto version = parsed->at("protocolVersion").get<std::uint32_t>();
    const auto heartbeat = parsed->at("heartbeatSeconds").get<std::uint32_t>();
    const QUrl websocket_url{QString::fromStdString(parsed->at("webSocketUrl").get<std::string>())};
    if (machine_id != hello.machine_id || version != protocol_version || heartbeat == 0U ||
        heartbeat > 3600U || !websocket_url.isValid() || websocket_url.scheme() != "ws")
        return Result<TransportSession>::failure(
            protocol_error("会话协商结果与 Uplink v1 不一致", "uplink.session.negotiate"));
    Result<void> websocket = Result<void>::failure(transport_error(
        "UPLINK_DISCONNECTED", "WebSocket 工作线程不可用", "uplink.websocket.connect", true));
    {
        std::scoped_lock lock{impl_->websocket_call_mutex};
        QMetaObject::invokeMethod(
            impl_->websocket_worker.get(),
            [this, &websocket, &websocket_url] {
                websocket =
                    impl_->websocket_worker->connect_to(websocket_url, impl_->config.io_timeout);
            },
            Qt::BlockingQueuedConnection);
    }
    if (!websocket)
        return Result<TransportSession>::failure(websocket.error());
    impl_->state.store(UplinkConnectionState::connected, std::memory_order_release);
    return Result<TransportSession>::success(
        {.session_id = parsed->at("sessionId").get<std::string>(),
         .machine_id = std::move(machine_id),
         .negotiated_protocol_version = version,
         .heartbeat_seconds = heartbeat});
}

void QtUplinkTransport::disconnect() noexcept
{
    impl_->cancel();
}

UplinkConnectionState QtUplinkTransport::connection_state() const noexcept
{
    return impl_->state.load(std::memory_order_acquire);
}

Result<TransportAcknowledgement> QtUplinkTransport::send_heartbeat(const MessageEnvelope& heartbeat)
{
    if (heartbeat.message_type != "heartbeat")
        return Result<TransportAcknowledgement>::failure(
            protocol_error("心跳消息类型无效", "uplink.heartbeat"));
    return impl_->websocket_send(heartbeat);
}

Result<TransportAcknowledgement> QtUplinkTransport::send_control_message(
    const MessageEnvelope& message)
{
    if (message.message_type == "heartbeat" || message.message_type == "command")
        return Result<TransportAcknowledgement>::failure(
            protocol_error("控制消息类型无效", "uplink.control"));
    return impl_->websocket_send(message);
}

Result<TransportAcknowledgement> QtUplinkTransport::send_event_metadata(
    const EventMetadataRequest& event)
{
    if (impl_->state.load(std::memory_order_acquire) != UplinkConnectionState::connected)
        return Result<TransportAcknowledgement>::failure(
            transport_error("UPLINK_DISCONNECTED", "上位机会话未连接", "uplink.event", true));
    if (auto valid = validate_identifier(event.request_id, "requestId", 128U); !valid)
        return Result<TransportAcknowledgement>::failure(valid.error());
    if (auto valid = validate_identifier(event.machine_id, "machineId", 64U); !valid)
        return Result<TransportAcknowledgement>::failure(valid.error());
    if (auto valid = validate_identifier(event.event_id, "eventId", 128U); !valid)
        return Result<TransportAcknowledgement>::failure(valid.error());
    auto body = Json::parse(event.metadata_json, nullptr, false);
    if (!body.is_object())
        return Result<TransportAcknowledgement>::failure(
            protocol_error("事件元数据必须是 JSON 对象", "uplink.event"));
    body["requestId"] = event.request_id;
    body["eventId"] = event.event_id;
    const std::string serialized = body.dump();
    if (serialized.size() > maximum_json_message_bytes)
        return Result<TransportAcknowledgement>::failure(
            protocol_error("事件元数据超过 1 MiB", "uplink.event"));
    const QString path =
        QStringLiteral("/api/uplink/v1/devices/%1/events/%2")
            .arg(QString::fromStdString(event.machine_id), QString::fromStdString(event.event_id));
    auto response =
        impl_->http("PUT", impl_->endpoint(path), QByteArray::fromStdString(serialized));
    if (!response)
        return Result<TransportAcknowledgement>::failure(response.error());
    if (response.value().status != 200 && response.value().status != 202)
        return Result<TransportAcknowledgement>::failure(
            response_error(response.value(), "uplink.event"));
    return Result<TransportAcknowledgement>::success(
        {.correlation_id = event.request_id, .acknowledged_at = current_utc_timestamp()});
}

Result<TransportAcknowledgement> QtUplinkTransport::upload_file(const UploadFileRequest& request)
{
    if (auto valid = validate_identifier(request.machine_id, "machineId", 64U); !valid)
        return Result<TransportAcknowledgement>::failure(valid.error());
    if (auto valid = validate_identifier(request.description.request_id, "requestId", 128U); !valid)
        return Result<TransportAcknowledgement>::failure(valid.error());
    if (auto valid = validate_identifier(request.description.event_id, "eventId", 128U); !valid)
        return Result<TransportAcknowledgement>::failure(valid.error());
    if (auto valid =
            validate_identifier(request.description.logical_file_id, "logicalFileId", 128U);
        !valid)
        return Result<TransportAcknowledgement>::failure(valid.error());
    if (request.description.total_bytes == 0U ||
        request.description.total_bytes > maximum_file_bytes ||
        request.description.chunk_bytes < 64U * 1024U ||
        request.description.chunk_bytes > maximum_chunk_bytes ||
        (!request.description.sha256.empty() && !is_sha256_hex(request.description.sha256)))
        return Result<TransportAcknowledgement>::failure(
            protocol_error("文件上传描述无效", "uplink.upload.validate"));

    QFile file{qstring_path(request.source_path)};
    const QFileInfo info{file};
    if (!info.isFile() || !file.open(QIODevice::ReadOnly))
        return Result<TransportAcknowledgement>::failure(transport_error(
            "UPLOAD_SOURCE_MISSING", "上传源文件不存在或不是普通文件", "uplink.upload.open"));
    if (static_cast<std::uint64_t>(file.size()) != request.description.total_bytes)
        return Result<TransportAcknowledgement>::failure(transport_error(
            "UPLOAD_SOURCE_CHANGED", "上传源文件长度与持久任务声明不一致", "uplink.upload.size"));
    auto digest = sha256_file(file, request.stop_token, request.description.total_bytes,
                              impl_->config.upload_limit_bytes_per_second);
    if (!digest)
        return Result<TransportAcknowledgement>::failure(digest.error());
    if (!request.description.sha256.empty() && digest.value() != request.description.sha256)
        return Result<TransportAcknowledgement>::failure(
            transport_error("UPLOAD_SOURCE_CHANGED", "上传源文件 SHA-256 与持久任务声明不一致",
                            "uplink.upload.sha256"));
    const std::string source_sha256 = std::move(digest).value();
    if (impl_->state.load(std::memory_order_acquire) != UplinkConnectionState::connected)
        return Result<TransportAcknowledgement>::failure(
            transport_error("UPLINK_DISCONNECTED", "上位机会话未连接", "uplink.upload", true));
    if (request.event_metadata)
    {
        if (request.event_metadata->machine_id != request.machine_id ||
            request.event_metadata->event_id != request.description.event_id)
            return Result<TransportAcknowledgement>::failure(
                protocol_error("文件与事件元数据标识不一致", "uplink.upload.metadata"));
        auto metadata = send_event_metadata(*request.event_metadata);
        if (!metadata)
            return Result<TransportAcknowledgement>::failure(metadata.error());
    }

    const QString uploads_path = QStringLiteral("/api/uplink/v1/devices/%1/uploads")
                                     .arg(QString::fromStdString(request.machine_id));
    const QByteArray create_body = QByteArray::fromStdString(Json{
        {"requestId", request.description.request_id},
        {"eventId", request.description.event_id},
        {"logicalFileId", request.description.logical_file_id},
        {"fileName", request.description.file_name},
        {"contentType", request.description.content_type},
        {"totalBytes", request.description.total_bytes},
        {"chunkBytes", request.description.chunk_bytes},
        {"sha256", source_sha256}}.dump());
    auto created =
        impl_->http("POST", impl_->endpoint(uploads_path), create_body, {}, request.stop_token);
    if (!created)
        return Result<TransportAcknowledgement>::failure(created.error());
    if (created.value().status != 200 && created.value().status != 201)
        return Result<TransportAcknowledgement>::failure(
            response_error(created.value(), "uplink.upload.create"));
    auto created_json = parse_object(created.value().body);
    if (!created_json || !created_json->contains("uploadId") ||
        !created_json->at("uploadId").is_string())
        return Result<TransportAcknowledgement>::failure(
            protocol_error("创建上传响应缺少 uploadId", "uplink.upload.create"));
    const std::string upload_id = created_json->at("uploadId").get<std::string>();
    if (auto valid = validate_identifier(upload_id, "uploadId", 128U); !valid)
        return Result<TransportAcknowledgement>::failure(valid.error());
    const QString upload_path = uploads_path + "/" + QString::fromStdString(upload_id);
    auto status = impl_->http("GET", impl_->endpoint(upload_path), {}, {}, request.stop_token);
    if (!status)
        return Result<TransportAcknowledgement>::failure(
            with_checkpoint(status.error(), upload_checkpoint(upload_id, {}, source_sha256)));
    if (status.value().status != 200)
        return Result<TransportAcknowledgement>::failure(
            with_checkpoint(response_error(status.value(), "uplink.upload.status"),
                            upload_checkpoint(upload_id, {}, source_sha256)));
    auto status_json = parse_object(status.value().body);
    if (!status_json || !status_json->contains("state") || !status_json->at("state").is_string() ||
        !status_json->contains("totalBytes") ||
        !status_json->at("totalBytes").is_number_unsigned() ||
        !status_json->contains("chunkBytes") ||
        !status_json->at("chunkBytes").is_number_unsigned() || !status_json->contains("sha256") ||
        !status_json->at("sha256").is_string() || !status_json->contains("receivedChunks") ||
        !status_json->at("receivedChunks").is_array())
        return Result<TransportAcknowledgement>::failure(
            protocol_error("上传状态响应字段无效", "uplink.upload.status"));
    if (status_json->at("totalBytes").get<std::uint64_t>() != request.description.total_bytes ||
        status_json->at("chunkBytes").get<std::uint32_t>() != request.description.chunk_bytes ||
        status_json->at("sha256").get<std::string>() != source_sha256)
        return Result<TransportAcknowledgement>::failure(transport_error(
            "UPLOAD_REJECTED", "服务端断点描述与本地持久任务冲突", "uplink.upload.status"));
    std::set<std::uint32_t> received;
    const auto total_chunks = static_cast<std::uint32_t>(
        (request.description.total_bytes + request.description.chunk_bytes - 1U) /
        request.description.chunk_bytes);
    if (status_json->at("receivedChunks").size() > total_chunks)
        return Result<TransportAcknowledgement>::failure(
            protocol_error("服务端断点分块数量越界", "uplink.upload.status"));
    for (const auto& chunk : status_json->at("receivedChunks"))
    {
        if (!chunk.is_number_unsigned())
            return Result<TransportAcknowledgement>::failure(
                protocol_error("服务端断点分块索引类型无效", "uplink.upload.status"));
        const auto index = chunk.get<std::uint32_t>();
        if (index >= total_chunks || !received.emplace(index).second)
            return Result<TransportAcknowledgement>::failure(
                protocol_error("服务端断点分块索引重复或越界", "uplink.upload.status"));
    }
    std::string checkpoint = upload_checkpoint(upload_id, received, source_sha256);
    if (status_json->at("state").get<std::string>() == "Completed")
        return Result<TransportAcknowledgement>::success(
            {.correlation_id = request.description.request_id,
             .acknowledged_at = current_utc_timestamp(),
             .checkpoint_json = std::move(checkpoint)});

    const auto transfer_start = std::chrono::steady_clock::now();
    std::uint64_t sent_bytes = 0U;
    for (std::uint32_t index = 0U; index < total_chunks; ++index)
    {
        if (received.contains(index))
            continue;
        if (request.stop_token.stop_requested())
            return Result<TransportAcknowledgement>::failure(
                with_checkpoint(transport_error("UPLOAD_TRANSFER_INTERRUPTED", "分块上传已取消",
                                                "uplink.upload.chunk", true),
                                checkpoint));
        const std::uint64_t offset =
            static_cast<std::uint64_t>(index) * request.description.chunk_bytes;
        const std::uint64_t length = std::min<std::uint64_t>(
            request.description.chunk_bytes, request.description.total_bytes - offset);
        if (!file.seek(static_cast<qint64>(offset)))
            return Result<TransportAcknowledgement>::failure(
                with_checkpoint(transport_error("UPLOAD_TRANSFER_FAILED", "无法定位上传分块",
                                                "uplink.upload.chunk.seek", true),
                                checkpoint));
        const QByteArray bytes = file.read(static_cast<qint64>(length));
        if (static_cast<std::uint64_t>(bytes.size()) != length)
            return Result<TransportAcknowledgement>::failure(
                with_checkpoint(transport_error("UPLOAD_SOURCE_CHANGED", "上传源文件发生短读",
                                                "uplink.upload.chunk.read"),
                                checkpoint));
        const QByteArray chunk_digest =
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
        const QByteArray range = QByteArray("bytes ") + QByteArray::number(offset) + "-" +
                                 QByteArray::number(offset + length - 1U) + "/" +
                                 QByteArray::number(request.description.total_bytes);
        const QString chunk_path = upload_path + "/chunks/" + QString::number(index);
        auto uploaded = impl_->http("PUT", impl_->endpoint(chunk_path), bytes,
                                    {{"content-type", "application/octet-stream"},
                                     {"content-range", range},
                                     {"x-chunk-sha256", chunk_digest}},
                                    request.stop_token);
        if (!uploaded)
            return Result<TransportAcknowledgement>::failure(
                with_checkpoint(uploaded.error(), checkpoint));
        if (uploaded.value().status != 200 && uploaded.value().status != 202)
            return Result<TransportAcknowledgement>::failure(with_checkpoint(
                response_error(uploaded.value(), "uplink.upload.chunk"), checkpoint));
        received.emplace(index);
        sent_bytes += length;
        checkpoint = upload_checkpoint(upload_id, received, source_sha256);
        const auto expected_elapsed = std::chrono::duration<long double>{
            static_cast<long double>(sent_bytes) /
            static_cast<long double>(impl_->config.upload_limit_bytes_per_second)};
        while (std::chrono::steady_clock::now() - transfer_start < expected_elapsed)
        {
            if (request.stop_token.stop_requested())
                return Result<TransportAcknowledgement>::failure(with_checkpoint(
                    transport_error("UPLOAD_TRANSFER_INTERRUPTED", "上传限速等待已取消",
                                    "uplink.upload.throttle", true),
                    checkpoint));
            QThread::msleep(1U);
        }
    }
    auto completed =
        impl_->http("POST", impl_->endpoint(upload_path + "/complete"), {}, {}, request.stop_token);
    if (!completed)
        return Result<TransportAcknowledgement>::failure(
            with_checkpoint(completed.error(), checkpoint));
    if (completed.value().status != 200)
        return Result<TransportAcknowledgement>::failure(with_checkpoint(
            response_error(completed.value(), "uplink.upload.complete"), checkpoint));
    return Result<TransportAcknowledgement>::success(
        {.correlation_id = request.description.request_id,
         .acknowledged_at = current_utc_timestamp(),
         .checkpoint_json = std::move(checkpoint)});
}

void QtUplinkTransport::set_command_handler(CommandHandler handler)
{
    std::scoped_lock lock{impl_->handler_mutex};
    impl_->command_handler = std::move(handler);
}

Result<UploadJobExecutor> make_chunked_upload_executor(std::shared_ptr<IUplinkTransport> transport,
                                                       ChunkedUploadExecutorConfig config)
{
    if (!transport || config.event_root.empty() ||
        !validate_identifier(config.machine_id, "machineId", 64U) ||
        config.chunk_bytes < 64U * 1024U || config.chunk_bytes > maximum_chunk_bytes)
        return Result<UploadJobExecutor>::failure(protocol_error(
            "分块上传执行器依赖、根目录、设备 ID 或分块无效", "uplink.executor.create"));
    return Result<UploadJobExecutor>::success(
        [transport = std::move(transport),
         config = std::move(config)](const storage::UploadJobRecord& job,
                                     const std::stop_token stop_token) -> UploadAttemptOutcome {
            if (job.kind == storage::UploadJobKind::alarm_metadata)
            {
                MessageEnvelope alarm{.protocol_version = protocol_version,
                                      .message_type = "alarm",
                                      .message_id = "alarm-" + std::to_string(job.job_id),
                                      .machine_id = config.machine_id,
                                      .sequence = static_cast<std::uint64_t>(job.job_id),
                                      .timestamp = current_utc_timestamp(),
                                      .payload_json = job.payload_json};
                auto sent = transport->send_control_message(alarm);
                if (sent)
                    return {.disposition = UploadAttemptDisposition::succeeded,
                            .checkpoint_json = job.checkpoint_json};
                return {.disposition = sent.error().retryable
                                           ? UploadAttemptDisposition::retryable_failure
                                           : UploadAttemptDisposition::permanent_failure,
                        .checkpoint_json = job.checkpoint_json,
                        .error_code = sent.error().business_code};
            }
            if (!job.event_id || !relative_path_is_safe(job.relative_path) ||
                job.logical_id.empty())
                return {.disposition = UploadAttemptDisposition::manual_intervention,
                        .checkpoint_json = job.checkpoint_json,
                        .error_code = "UPLOAD_JOB_INVALID"};
            const auto source = (config.event_root / job.relative_path).lexically_normal();
            std::error_code file_error;
            if (!std::filesystem::is_regular_file(source, file_error) || file_error)
                return {.disposition = UploadAttemptDisposition::manual_intervention,
                        .checkpoint_json = job.checkpoint_json,
                        .error_code = "UPLOAD_SOURCE_MISSING"};
            const auto size = std::filesystem::file_size(source, file_error);
            if (file_error || size == 0U || size != job.upload_bytes || size > maximum_file_bytes)
                return {.disposition = UploadAttemptDisposition::manual_intervention,
                        .checkpoint_json = job.checkpoint_json,
                        .error_code = "UPLOAD_SOURCE_CHANGED"};
            std::string declared = job.checksum;
            if (declared.starts_with("sha256:"))
                declared.erase(0U, 7U);
            if (!is_sha256_hex(declared))
                declared.clear();
            const auto file_name = utf8_path(source.filename());
            const std::string event_request_id =
                "event-" + QCryptographicHash::hash(QByteArray::fromStdString(*job.event_id),
                                                    QCryptographicHash::Sha256)
                               .toHex()
                               .toStdString();
            UploadFileRequest request{
                .machine_id = config.machine_id,
                .description = {.request_id = "upload-" + std::to_string(job.job_id),
                                .event_id = *job.event_id,
                                .logical_file_id = job.logical_id,
                                .file_name = file_name,
                                .content_type = content_type(job.kind, source),
                                .total_bytes = size,
                                .chunk_bytes = config.chunk_bytes,
                                .sha256 = std::move(declared)},
                .source_path = utf8_path(source),
                .event_metadata = EventMetadataRequest{.request_id = event_request_id,
                                                       .machine_id = config.machine_id,
                                                       .event_id = *job.event_id,
                                                       .metadata_json = job.payload_json},
                .checkpoint_json = job.checkpoint_json,
                .stop_token = stop_token};
            auto uploaded = transport->upload_file(request);
            if (uploaded)
                return {.disposition = UploadAttemptDisposition::succeeded,
                        .checkpoint_json = uploaded.value().checkpoint_json};
            const auto& error = uploaded.error();
            auto disposition = error.retryable ? UploadAttemptDisposition::retryable_failure
                                               : UploadAttemptDisposition::permanent_failure;
            if (error.business_code == "UPLOAD_SOURCE_MISSING" ||
                error.business_code == "UPLOAD_SOURCE_CHANGED")
                disposition = UploadAttemptDisposition::manual_intervention;
            return {.disposition = disposition,
                    .checkpoint_json = checkpoint_from_error(error, job.checkpoint_json),
                    .error_code = error.business_code};
        });
}

} // namespace paperbreak::uplink
