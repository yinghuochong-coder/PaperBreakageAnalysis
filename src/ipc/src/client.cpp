#include "paperbreak/ipc/client.hpp"

#include "paperbreak/common/error.hpp"

#include <QByteArray>
#include <QLocalSocket>
#include <QObject>
#include <QString>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <span>
#include <unordered_map>
#include <utility>

namespace paperbreak::ipc
{
namespace
{

Error client_error(std::string code, const Severity severity, std::string message,
                   std::string operation, const bool retryable = false)
{
    return make_error(std::move(code), severity, std::move(message), "ipc", std::move(operation),
                      retryable);
}

template <typename Callback, typename... Arguments>
void invoke_noexcept(const Callback& callback, Arguments&&... arguments) noexcept
{
    if (!callback)
    {
        return;
    }
    try
    {
        callback(std::forward<Arguments>(arguments)...);
    }
    catch (...)
    {
    }
}

} // namespace

class IpcClient::Impl final : public QObject
{
  public:
    Impl(IpcClientCallbacks callbacks, IpcClientOptions options)
        : callbacks_(std::move(callbacks)), options_(std::move(options)),
          owner_thread_(QThread::currentThread()), random_(std::random_device{}())
    {
        reconnect_timer_.setSingleShot(true);
        connect_timer_.setSingleShot(true);
        stable_timer_.setSingleShot(true);
        request_timer_.setSingleShot(true);
        QObject::connect(&reconnect_timer_, &QTimer::timeout, this, [this] { connect_now(); });
        QObject::connect(&connect_timer_, &QTimer::timeout, this, [this] {
            if (snapshot_.state == ClientConnectionState::connecting)
            {
                fail_connection_attempt("IPC 连接在截止时间内未完成", "ipc.client.connectTimeout");
            }
        });
        QObject::connect(&stable_timer_, &QTimer::timeout, this, [this] { reset_backoff(); });
        QObject::connect(&request_timer_, &QTimer::timeout, this, [this] { expire_requests(); });
    }

    ~Impl() override
    {
        stop();
    }

    [[nodiscard]] Result<void> start()
    {
        if (!on_owner_thread())
        {
            return Result<void>::failure(client_error("SYS_INTERNAL_ERROR", Severity::error,
                                                      "IPC 客户端必须在创建线程启动",
                                                      "ipc.client.start"));
        }
        if (started_)
        {
            return Result<void>::success();
        }
        if (!valid_options())
        {
            return Result<void>::failure(client_error("SYS_INTERNAL_ERROR", Severity::error,
                                                      "IPC 客户端选项无效", "ipc.client.start"));
        }
        started_ = true;
        reconnect_attempt_ = 0U;
        connect_now();
        return Result<void>::success();
    }

    void stop() noexcept
    {
        if (!started_)
        {
            return;
        }
        started_ = false;
        reconnect_timer_.stop();
        connect_timer_.stop();
        stable_timer_.stop();
        request_timer_.stop();
        retire_socket();
        decoder_.reset();
        snapshot_.state = ClientConnectionState::stopped;
        snapshot_.reconnect_attempt = 0U;
        snapshot_.next_retry_delay.reset();
        snapshot_.last_error.reset();
        notify_connection();
        fail_all(client_error("IPC_REQUEST_CANCELLED", Severity::info,
                              "IPC 客户端已停止，请求被取消", "ipc.client.stop"));
    }

    [[nodiscard]] ClientConnectionSnapshot snapshot() const
    {
        return snapshot_;
    }

    [[nodiscard]] Result<ClientRequestHandle> send_request(std::string command,
                                                           std::string payload_json,
                                                           std::vector<std::byte> binary,
                                                           RequestCompletion completion,
                                                           std::chrono::milliseconds timeout)
    {
        if (!on_owner_thread())
        {
            return Result<ClientRequestHandle>::failure(
                client_error("SYS_INTERNAL_ERROR", Severity::error, "IPC 请求必须在客户端线程发送",
                             "ipc.client.send"));
        }
        if (!started_ || snapshot_.state != ClientConnectionState::connected || !socket_ ||
            socket_->state() != QLocalSocket::ConnectedState)
        {
            return Result<ClientRequestHandle>::failure(
                client_error("IPC_NOT_CONNECTED", Severity::warning, "IPC 客户端当前未连接",
                             "ipc.client.send", true));
        }
        if (!completion)
        {
            return Result<ClientRequestHandle>::failure(client_error(
                "IPC_REQUEST_INVALID", Severity::error, "IPC 请求缺少完成回调", "ipc.client.send"));
        }
        if (pending_.size() >= options_.maximum_pending_requests)
        {
            return Result<ClientRequestHandle>::failure(client_error("IPC_BUSY", Severity::warning,
                                                                     "IPC 客户端在途请求已达上限",
                                                                     "ipc.client.send", true));
        }
        if (timeout == std::chrono::milliseconds::zero())
        {
            timeout = options_.default_request_timeout;
        }
        if (timeout < std::chrono::milliseconds::zero())
        {
            return Result<ClientRequestHandle>::failure(
                client_error("IPC_REQUEST_INVALID", Severity::error, "IPC 请求超时必须为正数",
                             "ipc.client.send"));
        }

        ClientRequestHandle handle{
            .request_id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(),
            .generation = snapshot_.generation};
        RequestMessage request{.request_id = handle.request_id,
                               .command = std::move(command),
                               .timestamp = current_utc_timestamp(),
                               .payload_json = std::move(payload_json),
                               .binary = std::move(binary)};
        auto frame = encode_request(request);
        if (!frame)
        {
            return Result<ClientRequestHandle>::failure(frame.error());
        }
        auto bytes = encode_frame(frame.value());
        if (!bytes)
        {
            return Result<ClientRequestHandle>::failure(bytes.error());
        }
        const auto queued_bytes =
            static_cast<std::size_t>(std::max<qint64>(socket_->bytesToWrite(), 0));
        if (bytes.value().size() > options_.outbound_byte_capacity -
                                       std::min(queued_bytes, options_.outbound_byte_capacity))
        {
            return Result<ClientRequestHandle>::failure(client_error("IPC_BUSY", Severity::warning,
                                                                     "IPC 客户端待发送数据已达上限",
                                                                     "ipc.client.send", true));
        }

        const QByteArray output{reinterpret_cast<const char*>(bytes.value().data()),
                                static_cast<qsizetype>(bytes.value().size())};
        if (socket_->write(output) != output.size())
        {
            Error error = connection_error("IPC 请求写入本地连接失败", "ipc.client.write");
            const std::uint64_t generation = generation_;
            schedule_reconnect(error);
            fail_generation(generation, error);
            return Result<ClientRequestHandle>::failure(std::move(error));
        }
        socket_->flush();
        pending_.emplace(handle.request_id,
                         Pending{.handle = handle,
                                 .deadline = std::chrono::steady_clock::now() + timeout,
                                 .completion = std::move(completion)});
        if (options_.diagnostics.enabled && options_.diagnostics.enabled() &&
            options_.diagnostics.record)
        {
            options_.diagnostics.record(
                "operation=console.ipc.send operationType=" + request.command + " correlationId=" +
                request.request_id + " jsonBytes=" + std::to_string(request.payload_json.size()) +
                " binaryBytes=" + std::to_string(request.binary.size()) +
                " frameBytes=" + std::to_string(bytes.value().size()) +
                " queueDepth=" + std::to_string(pending_.size()) + " result=accepted");
        }
        schedule_request_timer();
        return Result<ClientRequestHandle>::success(std::move(handle));
    }

    [[nodiscard]] bool cancel_request(const ClientRequestHandle& handle)
    {
        if (!on_owner_thread())
        {
            return false;
        }
        const auto iterator = pending_.find(handle.request_id);
        if (iterator == pending_.end() || iterator->second.handle.generation != handle.generation)
        {
            return false;
        }
        Pending pending = std::move(iterator->second);
        pending_.erase(iterator);
        schedule_request_timer();
        Error error = client_error("IPC_REQUEST_CANCELLED", Severity::info,
                                   "IPC 请求已由调用方取消", "ipc.client.cancel");
        error.correlation_id = handle.request_id;
        invoke_noexcept(pending.completion, pending.handle,
                        Result<ResponseMessage>::failure(std::move(error)));
        return true;
    }

  private:
    struct Pending final
    {
        ClientRequestHandle handle;
        std::chrono::steady_clock::time_point deadline;
        RequestCompletion completion;
    };

    [[nodiscard]] bool on_owner_thread() const noexcept
    {
        return QThread::currentThread() == owner_thread_;
    }

    [[nodiscard]] bool valid_options() const noexcept
    {
        return !options_.server_name.empty() && options_.maximum_pending_requests > 0U &&
               options_.outbound_byte_capacity > 0U &&
               options_.connect_timeout > std::chrono::milliseconds::zero() &&
               options_.default_request_timeout > std::chrono::milliseconds::zero() &&
               options_.initial_reconnect_delay > std::chrono::milliseconds::zero() &&
               options_.maximum_reconnect_delay >= options_.initial_reconnect_delay &&
               options_.stable_connection_reset > std::chrono::milliseconds::zero() &&
               options_.reconnect_jitter_fraction >= 0.0 &&
               options_.reconnect_jitter_fraction < 1.0;
    }

    void connect_now()
    {
        if (!started_)
        {
            return;
        }
        reconnect_timer_.stop();
        connect_timer_.stop();
        stable_timer_.stop();
        retire_socket();
        decoder_.reset();
        ++generation_;
        snapshot_.generation = generation_;
        snapshot_.state = ClientConnectionState::connecting;
        snapshot_.next_retry_delay.reset();
        snapshot_.reconnect_attempt = reconnect_attempt_;
        notify_connection();
        if (!started_ || generation_ != snapshot_.generation)
        {
            return;
        }

        socket_ = std::make_unique<QLocalSocket>(this);
        constexpr qint64 maximum_read_buffer =
            static_cast<qint64>(frame_prefix_bytes + maximum_header_bytes + maximum_binary_bytes);
        socket_->setReadBufferSize(maximum_read_buffer);
        const std::uint64_t generation = generation_;
        QObject::connect(socket_.get(), &QLocalSocket::connected, this,
                         [this, generation] { connected(generation); });
        QObject::connect(socket_.get(), &QLocalSocket::readyRead, this,
                         [this, generation] { read_available(generation); });
        QObject::connect(socket_.get(), &QLocalSocket::disconnected, this,
                         [this, generation] { disconnected(generation); });
        QObject::connect(
            socket_.get(), &QLocalSocket::errorOccurred, this,
            [this, generation](const QLocalSocket::LocalSocketError) { socket_error(generation); });
        socket_->connectToServer(QString::fromStdString(options_.server_name),
                                 QIODevice::ReadWrite);
        connect_timer_.start(options_.connect_timeout);
    }

    void connected(const std::uint64_t generation)
    {
        if (!current(generation) || snapshot_.state != ClientConnectionState::connecting)
        {
            return;
        }
        connect_timer_.stop();
        snapshot_.state = ClientConnectionState::connected;
        snapshot_.next_retry_delay.reset();
        snapshot_.last_error.reset();
        notify_connection();
        if (current(generation) && snapshot_.state == ClientConnectionState::connected)
        {
            stable_timer_.start(options_.stable_connection_reset);
        }
    }

    void socket_error(const std::uint64_t generation)
    {
        if (!current(generation) || snapshot_.state == ClientConnectionState::retry_wait)
        {
            return;
        }
        if (snapshot_.state == ClientConnectionState::connecting)
        {
            fail_connection_attempt("IPC 本地连接失败", "ipc.client.connect");
            return;
        }
        if (snapshot_.state == ClientConnectionState::connected)
        {
            Error error = connection_error("IPC 本地连接发生错误", "ipc.client.socketError");
            schedule_reconnect(error);
            fail_generation(generation, error);
        }
    }

    void disconnected(const std::uint64_t generation)
    {
        if (!current(generation) || snapshot_.state == ClientConnectionState::retry_wait)
        {
            return;
        }
        Error error = connection_error("IPC 本地连接已中断", "ipc.client.disconnect");
        schedule_reconnect(error);
        fail_generation(generation, error);
    }

    void fail_connection_attempt(const std::string& message, const std::string& operation)
    {
        schedule_reconnect(connection_error(message, operation));
    }

    [[nodiscard]] Error connection_error(const std::string& message,
                                         const std::string& operation) const
    {
        Error error =
            client_error("IPC_CONNECTION_LOST", Severity::warning, message, operation, true);
        if (socket_)
        {
            error.native_domain = "qt-local-socket";
            error.native_code = std::to_string(static_cast<int>(socket_->error()));
            const std::string text = socket_->errorString().toStdString();
            if (!text.empty())
            {
                error.details.push_back({"reason", text.substr(0U, 512U)});
            }
        }
        return error;
    }

    void schedule_reconnect(Error error)
    {
        if (!started_ || snapshot_.state == ClientConnectionState::retry_wait)
        {
            return;
        }
        connect_timer_.stop();
        stable_timer_.stop();
        retire_socket();
        decoder_.reset();
        ++reconnect_attempt_;
        const auto delay = reconnect_delay(reconnect_attempt_);
        snapshot_.state = ClientConnectionState::retry_wait;
        snapshot_.reconnect_attempt = reconnect_attempt_;
        snapshot_.next_retry_delay = delay;
        snapshot_.last_error = std::move(error);
        notify_connection();
        if (started_ && snapshot_.state == ClientConnectionState::retry_wait)
        {
            reconnect_timer_.start(delay);
        }
    }

    [[nodiscard]] std::chrono::milliseconds reconnect_delay(const std::size_t attempt)
    {
        const std::size_t exponent = std::min<std::size_t>(attempt > 0U ? attempt - 1U : 0U, 30U);
        const long double multiplier = static_cast<long double>(std::uint64_t{1} << exponent);
        const long double base = std::min(
            static_cast<long double>(options_.maximum_reconnect_delay.count()),
            static_cast<long double>(options_.initial_reconnect_delay.count()) * multiplier);
        std::uniform_real_distribution<double> jitter{-options_.reconnect_jitter_fraction,
                                                      options_.reconnect_jitter_fraction};
        const long double adjusted = base * (1.0L + static_cast<long double>(jitter(random_)));
        const auto bounded = static_cast<std::int64_t>(std::clamp(
            adjusted, 1.0L, static_cast<long double>(options_.maximum_reconnect_delay.count())));
        return std::chrono::milliseconds{bounded};
    }

    void read_available(const std::uint64_t generation)
    {
        if (!current(generation) || snapshot_.state != ClientConnectionState::connected || !socket_)
        {
            return;
        }
        constexpr qint64 chunk_size = 64 * 1024;
        std::size_t decoded_frames = 0U;
        constexpr std::size_t maximum_frames_per_turn = 64U;
        while (socket_ && socket_->bytesAvailable() > 0 && decoded_frames < maximum_frames_per_turn)
        {
            const QByteArray bytes = socket_->read(std::min(socket_->bytesAvailable(), chunk_size));
            if (bytes.isEmpty())
            {
                break;
            }
            const auto* begin = reinterpret_cast<const std::byte*>(bytes.constData());
            auto frames = decoder_.append(
                std::span<const std::byte>{begin, static_cast<std::size_t>(bytes.size())});
            if (!frames)
            {
                protocol_failure(frames.error());
                return;
            }
            for (const Frame& frame : frames.value())
            {
                ++decoded_frames;
                auto message = decode_server_message(frame);
                if (!message)
                {
                    protocol_failure(message.error());
                    return;
                }
                if (options_.diagnostics.enabled && options_.diagnostics.enabled() &&
                    options_.diagnostics.record)
                {
                    std::string correlation_id;
                    std::string operation_type;
                    std::size_t json_bytes = 0U;
                    std::size_t binary_bytes = 0U;
                    if (std::holds_alternative<ResponseMessage>(message.value()))
                    {
                        const auto& response = std::get<ResponseMessage>(message.value());
                        correlation_id = response.request_id;
                        operation_type = "response";
                        json_bytes = response.payload_json.size();
                        binary_bytes = response.binary.size();
                    }
                    else
                    {
                        const auto& push = std::get<PushMessage>(message.value());
                        correlation_id = push.coalescing_key;
                        operation_type = push.event_name;
                        json_bytes = push.payload_json.size();
                        binary_bytes = push.binary.size();
                    }
                    options_.diagnostics.record(
                        "operation=console.ipc.decode operationType=" + operation_type +
                        " correlationId=" + correlation_id +
                        " jsonBytes=" + std::to_string(json_bytes) +
                        " binaryBytes=" + std::to_string(binary_bytes) + " decodeResult=success");
                }
                reset_backoff();
                if (std::holds_alternative<ResponseMessage>(message.value()))
                {
                    receive_response(generation,
                                     std::get<ResponseMessage>(std::move(message).value()));
                }
                else
                {
                    const PushMessage push = std::get<PushMessage>(std::move(message).value());
                    invoke_noexcept(callbacks_.push_received, generation, push);
                }
                if (!current(generation) || snapshot_.state != ClientConnectionState::connected)
                {
                    return;
                }
            }
        }
        if (socket_ && socket_->bytesAvailable() > 0)
        {
            QTimer::singleShot(0, this, [this, generation] { read_available(generation); });
        }
    }

    void receive_response(const std::uint64_t generation, ResponseMessage response)
    {
        const auto iterator = pending_.find(response.request_id);
        if (iterator == pending_.end() || iterator->second.handle.generation != generation)
        {
            return;
        }
        Pending pending = std::move(iterator->second);
        pending_.erase(iterator);
        schedule_request_timer();
        invoke_noexcept(pending.completion, pending.handle,
                        Result<ResponseMessage>::success(std::move(response)));
    }

    void protocol_failure(Error error)
    {
        error.retryable = true;
        const std::uint64_t generation = generation_;
        Error request_error =
            connection_error("IPC 服务消息无效，连接已关闭", "ipc.client.protocolFailure");
        request_error.details.push_back({"cause", error.business_code});
        schedule_reconnect(std::move(error));
        fail_generation(generation, request_error);
    }

    void reset_backoff()
    {
        if (reconnect_attempt_ == 0U)
        {
            return;
        }
        reconnect_attempt_ = 0U;
        stable_timer_.stop();
        snapshot_.reconnect_attempt = 0U;
    }

    void expire_requests()
    {
        const auto now = std::chrono::steady_clock::now();
        std::vector<Pending> expired;
        for (auto iterator = pending_.begin(); iterator != pending_.end();)
        {
            if (iterator->second.deadline <= now)
            {
                expired.push_back(std::move(iterator->second));
                iterator = pending_.erase(iterator);
            }
            else
            {
                ++iterator;
            }
        }
        schedule_request_timer();
        for (Pending& pending : expired)
        {
            Error error = client_error("IPC_REQUEST_TIMEOUT", Severity::warning,
                                       "IPC 请求超过截止时间", "ipc.client.timeout", true);
            error.correlation_id = pending.handle.request_id;
            invoke_noexcept(pending.completion, pending.handle,
                            Result<ResponseMessage>::failure(std::move(error)));
        }
    }

    void schedule_request_timer()
    {
        request_timer_.stop();
        if (pending_.empty())
        {
            return;
        }
        const auto earliest = std::ranges::min_element(
            pending_, {}, [](const auto& item) { return item.second.deadline; });
        const auto remaining = earliest->second.deadline - std::chrono::steady_clock::now();
        const auto milliseconds = std::max(std::chrono::milliseconds{1},
                                           std::chrono::ceil<std::chrono::milliseconds>(remaining));
        request_timer_.start(milliseconds);
    }

    void fail_generation(const std::uint64_t generation, const Error& source)
    {
        std::vector<Pending> failed;
        for (auto iterator = pending_.begin(); iterator != pending_.end();)
        {
            if (iterator->second.handle.generation == generation)
            {
                failed.push_back(std::move(iterator->second));
                iterator = pending_.erase(iterator);
            }
            else
            {
                ++iterator;
            }
        }
        schedule_request_timer();
        for (Pending& pending : failed)
        {
            Error error = source;
            error.correlation_id = pending.handle.request_id;
            invoke_noexcept(pending.completion, pending.handle,
                            Result<ResponseMessage>::failure(std::move(error)));
        }
    }

    void fail_all(const Error& source)
    {
        std::vector<Pending> failed;
        failed.reserve(pending_.size());
        for (auto& [request_id, pending] : pending_)
        {
            static_cast<void>(request_id);
            failed.push_back(std::move(pending));
        }
        pending_.clear();
        for (Pending& pending : failed)
        {
            Error error = source;
            error.correlation_id = pending.handle.request_id;
            invoke_noexcept(pending.completion, pending.handle,
                            Result<ResponseMessage>::failure(std::move(error)));
        }
    }

    [[nodiscard]] bool current(const std::uint64_t generation) const noexcept
    {
        return started_ && generation == generation_;
    }

    void notify_connection() const noexcept
    {
        invoke_noexcept(callbacks_.connection_changed, snapshot_);
    }

    void retire_socket() noexcept
    {
        if (!socket_)
        {
            return;
        }
        QObject::disconnect(socket_.get(), nullptr, this, nullptr);
        socket_->abort();
        socket_->deleteLater();
        static_cast<void>(socket_.release());
    }

    IpcClientCallbacks callbacks_;
    IpcClientOptions options_;
    QThread* owner_thread_{};
    std::unique_ptr<QLocalSocket> socket_;
    QTimer reconnect_timer_;
    QTimer connect_timer_;
    QTimer stable_timer_;
    QTimer request_timer_;
    FrameDecoder decoder_;
    std::unordered_map<std::string, Pending> pending_;
    ClientConnectionSnapshot snapshot_;
    std::uint64_t generation_{};
    std::size_t reconnect_attempt_{};
    bool started_{};
    std::mt19937_64 random_;
};

IpcClient::IpcClient(IpcClientCallbacks callbacks, IpcClientOptions options)
    : impl_(std::make_unique<Impl>(std::move(callbacks), std::move(options)))
{
}

IpcClient::~IpcClient() = default;

Result<void> IpcClient::start()
{
    return impl_->start();
}

void IpcClient::stop() noexcept
{
    impl_->stop();
}

ClientConnectionSnapshot IpcClient::snapshot() const
{
    return impl_->snapshot();
}

Result<ClientRequestHandle> IpcClient::send_request(std::string command, std::string payload_json,
                                                    std::vector<std::byte> binary,
                                                    RequestCompletion completion,
                                                    const std::chrono::milliseconds timeout)
{
    return impl_->send_request(std::move(command), std::move(payload_json), std::move(binary),
                               std::move(completion), timeout);
}

bool IpcClient::cancel_request(const ClientRequestHandle& handle)
{
    return impl_->cancel_request(handle);
}

} // namespace paperbreak::ipc
