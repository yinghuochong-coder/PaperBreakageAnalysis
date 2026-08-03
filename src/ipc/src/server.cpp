#include "paperbreak/ipc/server.hpp"

#include "paperbreak/platform/local_ipc_security.hpp"

#include <QByteArray>
#include <QEventLoop>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMetaObject>
#include <QObject>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace paperbreak::ipc
{
namespace
{

Error server_error(std::string code, const Severity severity, std::string message,
                   std::string operation, const bool retryable = false)
{
    return make_error(std::move(code), severity, std::move(message), "ipc", std::move(operation),
                      retryable);
}

ResponseMessage failure_response(const std::string& request_id, Error error)
{
    error.correlation_id = request_id;
    return {.request_id = request_id,
            .success = false,
            .timestamp = current_utc_timestamp(),
            .payload_json = "{}",
            .error = std::move(error),
            .binary = {}};
}

class WindowsPeerAuthorizer final : public IPeerAuthorizer
{
  public:
    [[nodiscard]] Result<PeerIdentity> authorize(
        const std::uintptr_t native_descriptor) noexcept override
    {
        auto result = platform::inspect_local_named_pipe_peer(native_descriptor);
        if (!result)
        {
            return Result<PeerIdentity>::failure(result.error());
        }
        return Result<PeerIdentity>::success({.actor_sid = result.value().actor_sid,
                                              .local = result.value().local,
                                              .authenticated = result.value().authenticated,
                                              .administrator = result.value().administrator});
    }
};

} // namespace

std::unique_ptr<IPeerAuthorizer> make_windows_peer_authorizer()
{
    return std::make_unique<WindowsPeerAuthorizer>();
}

class IpcServer::Impl final
{
  public:
    Impl(std::shared_ptr<IRequestHandler> handler, std::unique_ptr<IPeerAuthorizer> authorizer,
         IpcServerOptions options)
        : handler_(std::move(handler)), authorizer_(std::move(authorizer)),
          options_(std::move(options)), event_thread_(*this)
    {
    }

    ~Impl()
    {
        request_stop();
        static_cast<void>(join(std::chrono::steady_clock::now() + std::chrono::seconds{5}));
    }

    [[nodiscard]] Result<void> start()
    {
        if (!handler_ || !authorizer_ || options_.server_name.empty() ||
            options_.instance_guard_name.empty() || options_.maximum_connections == 0U ||
            options_.maximum_connections >
                static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
            options_.maximum_in_flight_per_connection == 0U ||
            options_.recent_request_ids_per_connection == 0U ||
            options_.command_queue_capacity == 0U || options_.outbound_message_capacity == 0U ||
            options_.push_queue_capacity == 0U ||
            options_.push_queue_capacity > options_.outbound_message_capacity ||
            options_.outbound_byte_capacity == 0U || options_.publish_ingress_capacity == 0U ||
            options_.incomplete_frame_timeout <= std::chrono::milliseconds::zero() ||
            options_.startup_timeout <= std::chrono::milliseconds::zero() ||
            options_.shutdown_flush_timeout < std::chrono::milliseconds::zero())
        {
            return Result<void>::failure(server_error("SYS_INTERNAL_ERROR", Severity::error,
                                                      "IPC 服务端选项或依赖无效", "ipc.start"));
        }
        bool expected = false;
        if (!started_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        {
            return Result<void>::success();
        }

        auto guard = platform::NamedInstanceGuard::acquire(options_.instance_guard_name);
        if (!guard)
        {
            started_.store(false, std::memory_order_release);
            return Result<void>::failure(guard.error());
        }
        instance_guard_ = std::move(guard).value();
        accepting_commands_.store(true, std::memory_order_release);
        stopping_.store(false, std::memory_order_release);
        command_done_.store(false, std::memory_order_release);
        event_done_.store(false, std::memory_order_release);
        finish_scheduled_ = false;
        {
            std::scoped_lock lock{publish_mutex_};
            publish_queue_.clear();
            publish_posted_ = false;
        }
        {
            std::scoped_lock lock{startup_mutex_};
            startup_ready_ = false;
            startup_error_.reset();
        }

        command_thread_ =
            std::jthread([this](const std::stop_token token) { run_command_thread(token); });
        event_thread_.start();

        std::unique_lock lock{startup_mutex_};
        if (!startup_condition_.wait_for(lock, options_.startup_timeout,
                                         [this] { return startup_ready_; }))
        {
            lock.unlock();
            request_stop();
            static_cast<void>(join(std::chrono::steady_clock::now() + options_.startup_timeout));
            return Result<void>::failure(
                server_error("SYS_SERVICE_START_FAILED", Severity::critical,
                             "IPC 事件线程未在启动截止时间内就绪", "ipc.start"));
        }
        if (startup_error_.has_value())
        {
            const Error error = startup_error_.value();
            lock.unlock();
            request_stop();
            static_cast<void>(join(std::chrono::steady_clock::now() + options_.startup_timeout));
            return Result<void>::failure(error);
        }
        return Result<void>::success();
    }

    void request_stop() noexcept
    {
        if (!started_.load(std::memory_order_acquire))
        {
            return;
        }
        const bool already_stopping = stopping_.exchange(true, std::memory_order_acq_rel);
        accepting_commands_.store(false, std::memory_order_release);
        command_condition_.notify_all();
        if (!already_stopping)
        {
            static_cast<void>(post_event([this] { begin_event_stop(); }));
        }
    }

    [[nodiscard]] Result<void> join(const std::chrono::steady_clock::time_point deadline)
    {
        if (!started_.load(std::memory_order_acquire))
        {
            return Result<void>::success();
        }
        request_stop();
        {
            std::unique_lock lock{completion_mutex_};
            if (!completion_condition_.wait_until(lock, deadline, [this] {
                    return command_done_.load(std::memory_order_acquire) &&
                           event_done_.load(std::memory_order_acquire);
                }))
            {
                return Result<void>::failure(
                    server_error("SYS_SHUTDOWN_TIMEOUT", Severity::critical,
                                 "IPC 线程未在共享截止时间内停止", "ipc.join"));
            }
        }
        if (command_thread_.joinable())
        {
            command_thread_.join();
        }
        if (event_thread_.isRunning())
        {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0 || !event_thread_.wait(remaining.count()))
            {
                return Result<void>::failure(
                    server_error("SYS_SHUTDOWN_TIMEOUT", Severity::critical,
                                 "IPC Qt 事件线程未在共享截止时间内停止", "ipc.join"));
            }
        }
        instance_guard_.reset();
        started_.store(false, std::memory_order_release);
        return Result<void>::success();
    }

    [[nodiscard]] bool try_publish(PushMessage push, const PushPolicy policy) noexcept
    {
        try
        {
            if (!started_.load(std::memory_order_acquire) ||
                stopping_.load(std::memory_order_acquire))
            {
                return false;
            }
            bool schedule = false;
            {
                std::scoped_lock lock{publish_mutex_};
                if (publish_queue_.size() >= options_.publish_ingress_capacity)
                {
                    pushes_dropped_total_.fetch_add(1U, std::memory_order_relaxed);
                    return false;
                }
                publish_queue_.push_back({std::move(push), policy});
                publish_queue_depth_.store(publish_queue_.size(), std::memory_order_relaxed);
                update_high_watermark(publish_queue_high_watermark_, publish_queue_.size());
                if (!publish_posted_)
                {
                    publish_posted_ = true;
                    schedule = true;
                }
            }
            if (schedule && !post_event([this] { drain_publishes(); }))
            {
                std::scoped_lock lock{publish_mutex_};
                const auto dropped = static_cast<std::uint64_t>(publish_queue_.size());
                publish_queue_.clear();
                publish_queue_depth_.store(0U, std::memory_order_relaxed);
                publish_posted_ = false;
                pushes_dropped_total_.fetch_add(dropped, std::memory_order_relaxed);
                return false;
            }
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    [[nodiscard]] IpcServerMetrics metrics_snapshot() const noexcept
    {
        const std::uint64_t durations = completed_request_count_.load(std::memory_order_relaxed);
        const std::uint64_t total_microseconds =
            request_duration_total_us_.load(std::memory_order_relaxed);
        return {.active_connections = active_connections_.load(std::memory_order_relaxed),
                .in_flight_requests = in_flight_requests_.load(std::memory_order_relaxed),
                .command_queue_depth = command_queue_depth_.load(std::memory_order_relaxed),
                .command_queue_high_watermark =
                    command_queue_high_watermark_.load(std::memory_order_relaxed),
                .publish_queue_depth = publish_queue_depth_.load(std::memory_order_relaxed),
                .publish_queue_high_watermark =
                    publish_queue_high_watermark_.load(std::memory_order_relaxed),
                .outbound_messages = outbound_messages_.load(std::memory_order_relaxed),
                .outbound_bytes = outbound_bytes_.load(std::memory_order_relaxed),
                .requests_total = requests_total_.load(std::memory_order_relaxed),
                .responses_total = responses_total_.load(std::memory_order_relaxed),
                .protocol_errors_total = protocol_errors_total_.load(std::memory_order_relaxed),
                .pushes_dropped_total = pushes_dropped_total_.load(std::memory_order_relaxed),
                .average_request_duration_ms = durations == 0U
                                                   ? 0.0
                                                   : static_cast<double>(total_microseconds) /
                                                         static_cast<double>(durations) / 1000.0,
                .maximum_request_duration_ms =
                    static_cast<double>(request_duration_max_us_.load(std::memory_order_relaxed)) /
                    1000.0};
    }

  private:
    struct QueuedCommand final
    {
        std::uint64_t connection_id{};
        RequestMessage request;
        PeerIdentity peer;
        std::chrono::steady_clock::time_point queued_at;
    };

    static void update_high_watermark(std::atomic_uint64_t& target,
                                      const std::size_t value) noexcept
    {
        std::uint64_t previous = target.load(std::memory_order_relaxed);
        const auto candidate = static_cast<std::uint64_t>(value);
        while (candidate > previous &&
               !target.compare_exchange_weak(previous, candidate, std::memory_order_relaxed))
        {
        }
    }

    void update_outbound_metrics() noexcept
    {
        std::uint64_t messages = 0U;
        std::uint64_t bytes = 0U;
        for (const auto& [identifier, client] : clients_)
        {
            static_cast<void>(identifier);
            messages +=
                static_cast<std::uint64_t>(client->responses.size() + client->pushes.size() +
                                           (client->active.has_value() ? 1U : 0U));
            bytes += static_cast<std::uint64_t>(client->outbound_bytes);
        }
        outbound_messages_.store(messages, std::memory_order_relaxed);
        outbound_bytes_.store(bytes, std::memory_order_relaxed);
    }

    struct QueuedPublish final
    {
        PushMessage push;
        PushPolicy policy{PushPolicy::drop_newest};
    };

    enum class OutboundKind
    {
        response,
        push,
    };

    struct OutboundItem final
    {
        QByteArray bytes;
        OutboundKind kind{OutboundKind::response};
        std::string key;
    };

    struct Client final
    {
        QLocalSocket* socket{};
        FrameDecoder decoder;
        std::optional<std::chrono::steady_clock::time_point> frame_deadline;
        std::optional<PeerIdentity> peer;
        std::size_t in_flight{};
        std::deque<std::string> recent_request_order;
        std::unordered_set<std::string> recent_request_ids;
        std::deque<OutboundItem> responses;
        std::deque<OutboundItem> pushes;
        std::optional<OutboundItem> active;
        std::size_t outbound_bytes{};
        bool close_after_flush{};
    };

    class EventThread final : public QThread
    {
      public:
        explicit EventThread(Impl& owner) : owner_(owner) {}

      protected:
        void run() override
        {
            owner_.run_event_thread();
        }

      private:
        Impl& owner_;
    };

    template <typename Function> [[nodiscard]] bool post_event(Function&& function) noexcept
    {
        try
        {
            std::scoped_lock lock{event_pointer_mutex_};
            if (event_dispatcher_ == nullptr)
            {
                return false;
            }
            return QMetaObject::invokeMethod(event_dispatcher_, std::forward<Function>(function),
                                             Qt::QueuedConnection);
        }
        catch (...)
        {
            return false;
        }
    }

    void signal_startup(std::optional<Error> error)
    {
        {
            std::scoped_lock lock{startup_mutex_};
            startup_error_ = std::move(error);
            startup_ready_ = true;
        }
        startup_condition_.notify_all();
    }

    void run_event_thread()
    {
        QObject dispatcher;
        QEventLoop loop;
        QLocalServer server;
        QTimer timeout_timer;
        {
            std::scoped_lock lock{event_pointer_mutex_};
            event_dispatcher_ = &dispatcher;
            event_loop_ = &loop;
            local_server_ = &server;
        }

        server.setSocketOptions(QLocalServer::WorldAccessOption);
        server.setMaxPendingConnections(static_cast<int>(options_.maximum_connections));
        server.setListenBacklogSize(static_cast<int>(options_.maximum_connections));
        QObject::connect(&server, &QLocalServer::newConnection, &dispatcher,
                         [this] { accept_connections(); });

        const int timer_interval = static_cast<int>(
            std::clamp<std::int64_t>(options_.incomplete_frame_timeout.count() / 4, 10, 250));
        timeout_timer.setInterval(timer_interval);
        QObject::connect(&timeout_timer, &QTimer::timeout, &dispatcher,
                         [this] { check_frame_timeouts(); });

        if (!server.listen(QString::fromUtf8(options_.server_name)))
        {
            Error error = server_error("SYS_SERVICE_START_FAILED", Severity::critical,
                                       "QLocalServer 无法监听 IPC 端点", "ipc.listen");
            error.native_domain = "qt-network";
            error.native_code = std::to_string(static_cast<int>(server.serverError()));
            error.details.push_back({"qtError", server.errorString().toStdString()});
            signal_startup(std::move(error));
        }
        else
        {
            timeout_timer.start();
            signal_startup(std::nullopt);
            if (stopping_.load(std::memory_order_acquire))
            {
                begin_event_stop();
            }
            loop.exec();
        }

        timeout_timer.stop();
        server.close();
        for (auto& [identifier, client] : clients_)
        {
            static_cast<void>(identifier);
            QObject::disconnect(client->socket, nullptr, event_dispatcher_, nullptr);
            client->socket->abort();
            client->socket->deleteLater();
        }
        clients_.clear();
        active_connections_.store(0U, std::memory_order_relaxed);
        in_flight_requests_.store(0U, std::memory_order_relaxed);
        update_outbound_metrics();
        {
            std::scoped_lock lock{event_pointer_mutex_};
            event_dispatcher_ = nullptr;
            event_loop_ = nullptr;
            local_server_ = nullptr;
        }
        event_done_.store(true, std::memory_order_release);
        completion_condition_.notify_all();
    }

    void accept_connections()
    {
        while (local_server_ != nullptr && local_server_->hasPendingConnections())
        {
            QLocalSocket* socket = local_server_->nextPendingConnection();
            if (socket == nullptr)
            {
                continue;
            }
            socket->setReadBufferSize(64 * 1024);
            socket->setParent(event_dispatcher_);
            if (stopping_.load(std::memory_order_acquire) ||
                clients_.size() >= options_.maximum_connections)
            {
                socket->abort();
                socket->deleteLater();
                continue;
            }
            const std::uint64_t identifier = next_connection_id_++;
            auto client = std::make_unique<Client>();
            client->socket = socket;
            clients_.emplace(identifier, std::move(client));
            active_connections_.store(clients_.size(), std::memory_order_relaxed);
            QObject::connect(socket, &QLocalSocket::readyRead, event_dispatcher_,
                             [this, identifier] { read_client(identifier); });
            QObject::connect(socket, &QLocalSocket::bytesWritten, event_dispatcher_,
                             [this, identifier](const qint64) { bytes_written(identifier); });
            QObject::connect(socket, &QLocalSocket::disconnected, event_dispatcher_,
                             [this, identifier] { remove_client(identifier); });
            QObject::connect(socket, &QLocalSocket::errorOccurred, event_dispatcher_,
                             [this, identifier](const QLocalSocket::LocalSocketError) {
                                 auto iterator = clients_.find(identifier);
                                 if (iterator != clients_.end() &&
                                     iterator->second->socket->state() ==
                                         QLocalSocket::UnconnectedState)
                                 {
                                     remove_client(identifier);
                                 }
                             });
        }
    }

    void read_client(const std::uint64_t identifier)
    {
        auto iterator = clients_.find(identifier);
        if (iterator == clients_.end())
        {
            return;
        }
        Client& client = *iterator->second;
        while (client.socket->bytesAvailable() > 0)
        {
            const QByteArray chunk = client.socket->read(64 * 1024);
            if (chunk.isEmpty())
            {
                break;
            }
            if (!client.decoder.has_pending_data())
            {
                client.frame_deadline =
                    std::chrono::steady_clock::now() + options_.incomplete_frame_timeout;
            }
            const auto bytes =
                std::span<const std::byte>{reinterpret_cast<const std::byte*>(chunk.constData()),
                                           static_cast<std::size_t>(chunk.size())};
            auto decoded = client.decoder.append(bytes);
            if (!decoded)
            {
                protocol_errors_total_.fetch_add(1U, std::memory_order_relaxed);
                abort_client(identifier);
                return;
            }
            for (const Frame& frame : decoded.value())
            {
                handle_frame(identifier, frame);
                if (!clients_.contains(identifier))
                {
                    return;
                }
            }
            if (!client.decoder.has_pending_data())
            {
                client.frame_deadline.reset();
            }
        }
    }

    void handle_frame(const std::uint64_t identifier, const Frame& frame)
    {
        auto iterator = clients_.find(identifier);
        if (iterator == clients_.end())
        {
            return;
        }
        Client& client = *iterator->second;
        auto request = decode_request(frame);
        if (!request)
        {
            protocol_errors_total_.fetch_add(1U, std::memory_order_relaxed);
            const Error& error = request.error();
            if (!error.correlation_id.has_value())
            {
                abort_client(identifier);
                return;
            }
            if (!ensure_peer(identifier))
            {
                return;
            }
            const bool close = error.business_code == "IPC_PROTOCOL_VERSION_UNSUPPORTED";
            enqueue_response(identifier, failure_response(error.correlation_id.value(), error),
                             close);
            return;
        }
        if (!ensure_peer(identifier))
        {
            return;
        }
        if (stopping_.load(std::memory_order_acquire))
        {
            enqueue_response(
                identifier,
                failure_response(request.value().request_id,
                                 server_error("SYS_SERVICE_STOPPING", Severity::warning,
                                              "服务正在停止，拒绝新的 IPC 请求", "ipc.dispatch",
                                              true)),
                false);
            return;
        }
        if (client.recent_request_ids.contains(request.value().request_id))
        {
            enqueue_response(
                identifier,
                failure_response(request.value().request_id,
                                 server_error("IPC_REQUEST_CONFLICT", Severity::warning,
                                              "requestId 已在当前连接使用", "ipc.dispatch")),
                false);
            return;
        }
        remember_request_id(client, request.value().request_id);
        if (client.in_flight >= options_.maximum_in_flight_per_connection)
        {
            enqueue_response(
                identifier,
                failure_response(request.value().request_id,
                                 server_error("IPC_BUSY", Severity::warning,
                                              "客户端在途请求达到上限", "ipc.dispatch", true)),
                false);
            return;
        }

        PeerIdentity peer = client.peer.value();
        peer.connection_id = identifier;
        QueuedCommand command{.connection_id = identifier,
                              .request = std::move(request).value(),
                              .peer = std::move(peer),
                              .queued_at = std::chrono::steady_clock::now()};
        {
            std::scoped_lock lock{command_mutex_};
            if (!accepting_commands_.load(std::memory_order_acquire) ||
                command_queue_.size() >= options_.command_queue_capacity)
            {
                enqueue_response(
                    identifier,
                    failure_response(command.request.request_id,
                                     server_error("IPC_BUSY", Severity::warning, "IPC 命令队列已满",
                                                  "ipc.dispatch", true)),
                    false);
                return;
            }
            ++client.in_flight;
            command_queue_.push_back(std::move(command));
            requests_total_.fetch_add(1U, std::memory_order_relaxed);
            in_flight_requests_.fetch_add(1U, std::memory_order_relaxed);
            command_queue_depth_.store(command_queue_.size(), std::memory_order_relaxed);
            update_high_watermark(command_queue_high_watermark_, command_queue_.size());
        }
        command_condition_.notify_one();
    }

    bool ensure_peer(const std::uint64_t identifier)
    {
        auto iterator = clients_.find(identifier);
        if (iterator == clients_.end())
        {
            return false;
        }
        Client& client = *iterator->second;
        if (client.peer.has_value())
        {
            return true;
        }
        auto peer =
            authorizer_->authorize(static_cast<std::uintptr_t>(client.socket->socketDescriptor()));
        if (!peer || !peer.value().local || !peer.value().authenticated)
        {
            abort_client(identifier);
            return false;
        }
        client.peer = std::move(peer).value();
        return true;
    }

    void remember_request_id(Client& client, const std::string& request_id)
    {
        client.recent_request_ids.insert(request_id);
        client.recent_request_order.push_back(request_id);
        while (client.recent_request_order.size() > options_.recent_request_ids_per_connection)
        {
            client.recent_request_ids.erase(client.recent_request_order.front());
            client.recent_request_order.pop_front();
        }
    }

    void run_command_thread(const std::stop_token stop_token)
    {
        while (true)
        {
            QueuedCommand command;
            {
                std::unique_lock lock{command_mutex_};
                command_condition_.wait(lock, stop_token, [this] {
                    return !command_queue_.empty() ||
                           !accepting_commands_.load(std::memory_order_acquire);
                });
                if (command_queue_.empty())
                {
                    if (!accepting_commands_.load(std::memory_order_acquire) ||
                        stop_token.stop_requested())
                    {
                        break;
                    }
                    continue;
                }
                command = std::move(command_queue_.front());
                command_queue_.pop_front();
                command_queue_depth_.store(command_queue_.size(), std::memory_order_relaxed);
            }

            Result<CommandResponse> handled = Result<CommandResponse>::failure(server_error(
                "SYS_INTERNAL_ERROR", Severity::error, "IPC 命令处理未产生结果", "ipc.command"));
            try
            {
                handled = handler_->handle(command.request, command.peer, stop_token);
            }
            catch (...)
            {
                handled = Result<CommandResponse>::failure(
                    server_error("SYS_INTERNAL_ERROR", Severity::error,
                                 "IPC 命令处理器抛出了未处理异常", "ipc.command"));
            }

            ResponseMessage response;
            response.request_id = command.request.request_id;
            response.timestamp = current_utc_timestamp();
            if (handled)
            {
                response.success = true;
                response.payload_json = handled.value().payload_json;
                response.binary = std::move(handled).value().binary;
            }
            else
            {
                response.success = false;
                response.payload_json = "{}";
                Error error = handled.error();
                error.correlation_id = command.request.request_id;
                response.error = std::move(error);
            }
            const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - command.queued_at);
            const auto duration_us =
                static_cast<std::uint64_t>(std::max<std::int64_t>(duration.count(), 0));
            request_duration_total_us_.fetch_add(duration_us, std::memory_order_relaxed);
            completed_request_count_.fetch_add(1U, std::memory_order_relaxed);
            update_high_watermark(request_duration_max_us_, static_cast<std::size_t>(duration_us));
            static_cast<void>(post_event([this, identifier = command.connection_id,
                                          response = std::move(response)]() mutable {
                auto iterator = clients_.find(identifier);
                if (iterator != clients_.end() && iterator->second->in_flight > 0U)
                {
                    --iterator->second->in_flight;
                    in_flight_requests_.fetch_sub(1U, std::memory_order_relaxed);
                    enqueue_response(identifier, std::move(response), false);
                }
            }));
        }

        command_done_.store(true, std::memory_order_release);
        completion_condition_.notify_all();
        static_cast<void>(post_event([this] { maybe_finish_event_stop(); }));
    }

    void enqueue_response(const std::uint64_t identifier, ResponseMessage response,
                          const bool close_after)
    {
        auto frame = encode_response(response);
        if (!frame)
        {
            abort_client(identifier);
            return;
        }
        auto encoded = encode_frame(frame.value());
        if (!encoded)
        {
            abort_client(identifier);
            return;
        }
        QByteArray bytes{reinterpret_cast<const char*>(encoded.value().data()),
                         static_cast<qsizetype>(encoded.value().size())};
        auto iterator = clients_.find(identifier);
        if (iterator == clients_.end())
        {
            return;
        }
        Client& client = *iterator->second;
        if (!can_enqueue(client, static_cast<std::size_t>(bytes.size()), OutboundKind::response))
        {
            abort_client(identifier);
            return;
        }
        client.outbound_bytes += static_cast<std::size_t>(bytes.size());
        client.responses.push_back(
            {.bytes = std::move(bytes), .kind = OutboundKind::response, .key = {}});
        responses_total_.fetch_add(1U, std::memory_order_relaxed);
        update_outbound_metrics();
        client.close_after_flush = client.close_after_flush || close_after;
        pump_client(identifier);
    }

    bool can_enqueue(const Client& client, const std::size_t bytes, const OutboundKind kind) const
    {
        const std::size_t messages =
            client.responses.size() + client.pushes.size() + (client.active.has_value() ? 1U : 0U);
        if (messages >= options_.outbound_message_capacity ||
            bytes > options_.outbound_byte_capacity -
                        std::min(client.outbound_bytes, options_.outbound_byte_capacity))
        {
            return false;
        }
        if (kind == OutboundKind::push)
        {
            const std::size_t pushes =
                client.pushes.size() +
                (client.active.has_value() && client.active->kind == OutboundKind::push ? 1U : 0U);
            return pushes < options_.push_queue_capacity;
        }
        return true;
    }

    void enqueue_push(Client& client, const QByteArray& bytes, const PushMessage& push,
                      const PushPolicy policy)
    {
        const std::string key = push.coalescing_key.empty() ? push.event_name : push.coalescing_key;
        if (policy == PushPolicy::coalesce_latest)
        {
            const auto existing =
                std::find_if(client.pushes.begin(), client.pushes.end(),
                             [&key](const OutboundItem& item) { return item.key == key; });
            if (existing != client.pushes.end())
            {
                const std::size_t previous = static_cast<std::size_t>(existing->bytes.size());
                const std::size_t replacement = static_cast<std::size_t>(bytes.size());
                const std::size_t without_previous = client.outbound_bytes - previous;
                if (replacement <= options_.outbound_byte_capacity -
                                       std::min(without_previous, options_.outbound_byte_capacity))
                {
                    client.outbound_bytes = without_previous + replacement;
                    existing->bytes = bytes;
                    update_outbound_metrics();
                }
                else
                {
                    pushes_dropped_total_.fetch_add(1U, std::memory_order_relaxed);
                }
                return;
            }
        }
        if (!can_enqueue(client, static_cast<std::size_t>(bytes.size()), OutboundKind::push))
        {
            pushes_dropped_total_.fetch_add(1U, std::memory_order_relaxed);
            return;
        }
        client.outbound_bytes += static_cast<std::size_t>(bytes.size());
        client.pushes.push_back({.bytes = bytes, .kind = OutboundKind::push, .key = key});
        update_outbound_metrics();
    }

    void drain_publishes()
    {
        while (true)
        {
            QueuedPublish queued;
            {
                std::scoped_lock lock{publish_mutex_};
                if (publish_queue_.empty())
                {
                    publish_posted_ = false;
                    break;
                }
                queued = std::move(publish_queue_.front());
                publish_queue_.pop_front();
                publish_queue_depth_.store(publish_queue_.size(), std::memory_order_relaxed);
            }
            publish_on_event(queued.push, queued.policy);
        }
    }

    void publish_on_event(const PushMessage& push, const PushPolicy policy)
    {
        auto frame = encode_push(push);
        if (!frame)
        {
            return;
        }
        auto encoded = encode_frame(frame.value());
        if (!encoded)
        {
            return;
        }
        const QByteArray bytes{reinterpret_cast<const char*>(encoded.value().data()),
                               static_cast<qsizetype>(encoded.value().size())};
        std::vector<std::uint64_t> identifiers;
        identifiers.reserve(clients_.size());
        for (auto& [identifier, client] : clients_)
        {
            if (push.target_connection_id.has_value() &&
                push.target_connection_id.value() != identifier)
            {
                continue;
            }
            enqueue_push(*client, bytes, push, policy);
            identifiers.push_back(identifier);
        }
        for (const std::uint64_t identifier : identifiers)
        {
            pump_client(identifier);
        }
    }

    void pump_client(const std::uint64_t identifier)
    {
        auto iterator = clients_.find(identifier);
        if (iterator == clients_.end())
        {
            return;
        }
        Client& client = *iterator->second;
        if (client.active.has_value() || client.socket->bytesToWrite() > 0)
        {
            return;
        }
        if (!client.responses.empty())
        {
            client.active = std::move(client.responses.front());
            client.responses.pop_front();
        }
        else if (!client.pushes.empty())
        {
            client.active = std::move(client.pushes.front());
            client.pushes.pop_front();
        }
        else
        {
            if (client.close_after_flush)
            {
                client.socket->disconnectFromServer();
            }
            return;
        }
        if (client.socket->write(client.active->bytes) != client.active->bytes.size())
        {
            abort_client(identifier);
            return;
        }
        client.socket->flush();
        QTimer::singleShot(0, event_dispatcher_, [this, identifier] { bytes_written(identifier); });
    }

    void bytes_written(const std::uint64_t identifier)
    {
        auto iterator = clients_.find(identifier);
        if (iterator == clients_.end())
        {
            return;
        }
        Client& client = *iterator->second;
        if (client.active.has_value() && client.socket->bytesToWrite() == 0)
        {
            client.outbound_bytes -= static_cast<std::size_t>(client.active->bytes.size());
            client.active.reset();
            update_outbound_metrics();
            pump_client(identifier);
        }
    }

    void check_frame_timeouts()
    {
        const auto now = std::chrono::steady_clock::now();
        std::vector<std::uint64_t> expired;
        for (const auto& [identifier, client] : clients_)
        {
            if (client->frame_deadline.has_value() && now >= client->frame_deadline.value())
            {
                expired.push_back(identifier);
            }
        }
        for (const std::uint64_t identifier : expired)
        {
            abort_client(identifier);
        }
    }

    void begin_event_stop()
    {
        if (local_server_ != nullptr)
        {
            local_server_->close();
        }
        publish_on_event({.event_name = "status.changed",
                          .timestamp = current_utc_timestamp(),
                          .payload_json = R"({"serviceState":"stop-requested"})",
                          .binary = {},
                          .coalescing_key = "status.changed"},
                         PushPolicy::coalesce_latest);
        maybe_finish_event_stop();
    }

    void maybe_finish_event_stop()
    {
        if (!stopping_.load(std::memory_order_acquire) ||
            !command_done_.load(std::memory_order_acquire) || finish_scheduled_ ||
            event_loop_ == nullptr)
        {
            return;
        }
        finish_scheduled_ = true;
        QTimer::singleShot(options_.shutdown_flush_timeout, event_dispatcher_, [this] {
            std::vector<std::uint64_t> identifiers;
            identifiers.reserve(clients_.size());
            for (const auto& [identifier, client] : clients_)
            {
                static_cast<void>(client);
                identifiers.push_back(identifier);
            }
            for (const std::uint64_t identifier : identifiers)
            {
                abort_client(identifier);
            }
            if (event_loop_ != nullptr)
            {
                event_loop_->quit();
            }
        });
    }

    void abort_client(const std::uint64_t identifier)
    {
        const auto iterator = clients_.find(identifier);
        if (iterator != clients_.end())
        {
            iterator->second->socket->abort();
        }
    }

    void remove_client(const std::uint64_t identifier)
    {
        const auto iterator = clients_.find(identifier);
        if (iterator == clients_.end())
        {
            return;
        }
        iterator->second->socket->deleteLater();
        const std::size_t client_in_flight = iterator->second->in_flight;
        clients_.erase(iterator);
        active_connections_.store(clients_.size(), std::memory_order_relaxed);
        if (client_in_flight > 0U)
        {
            in_flight_requests_.fetch_sub(client_in_flight, std::memory_order_relaxed);
        }
        update_outbound_metrics();
    }

    std::shared_ptr<IRequestHandler> handler_;
    std::unique_ptr<IPeerAuthorizer> authorizer_;
    IpcServerOptions options_;
    std::unique_ptr<platform::NamedInstanceGuard> instance_guard_;

    std::atomic_bool started_{false};
    std::atomic_bool stopping_{false};
    std::atomic_bool accepting_commands_{false};
    std::atomic_bool command_done_{true};
    std::atomic_bool event_done_{true};

    std::mutex startup_mutex_;
    std::condition_variable startup_condition_;
    bool startup_ready_{};
    std::optional<Error> startup_error_;

    std::mutex completion_mutex_;
    std::condition_variable completion_condition_;

    std::mutex command_mutex_;
    std::condition_variable_any command_condition_;
    std::deque<QueuedCommand> command_queue_;
    std::jthread command_thread_;

    std::mutex publish_mutex_;
    std::deque<QueuedPublish> publish_queue_;
    bool publish_posted_{};

    std::mutex event_pointer_mutex_;
    QObject* event_dispatcher_{};
    QEventLoop* event_loop_{};
    QLocalServer* local_server_{};
    EventThread event_thread_;

    std::unordered_map<std::uint64_t, std::unique_ptr<Client>> clients_;
    std::uint64_t next_connection_id_{1U};
    bool finish_scheduled_{};

    std::atomic_uint64_t active_connections_{};
    std::atomic_uint64_t in_flight_requests_{};
    std::atomic_uint64_t command_queue_depth_{};
    std::atomic_uint64_t command_queue_high_watermark_{};
    std::atomic_uint64_t publish_queue_depth_{};
    std::atomic_uint64_t publish_queue_high_watermark_{};
    std::atomic_uint64_t outbound_messages_{};
    std::atomic_uint64_t outbound_bytes_{};
    std::atomic_uint64_t requests_total_{};
    std::atomic_uint64_t responses_total_{};
    std::atomic_uint64_t protocol_errors_total_{};
    std::atomic_uint64_t pushes_dropped_total_{};
    std::atomic_uint64_t completed_request_count_{};
    std::atomic_uint64_t request_duration_total_us_{};
    std::atomic_uint64_t request_duration_max_us_{};
};

IpcServer::IpcServer(std::shared_ptr<IRequestHandler> handler,
                     std::unique_ptr<IPeerAuthorizer> authorizer, IpcServerOptions options)
    : impl_(std::make_unique<Impl>(std::move(handler), std::move(authorizer), std::move(options)))
{
}

IpcServer::~IpcServer() = default;

Result<void> IpcServer::start()
{
    return impl_->start();
}

void IpcServer::request_stop() noexcept
{
    impl_->request_stop();
}

Result<void> IpcServer::join(const std::chrono::steady_clock::time_point deadline)
{
    return impl_->join(deadline);
}

bool IpcServer::try_publish(PushMessage push, const PushPolicy policy) noexcept
{
    return impl_->try_publish(std::move(push), policy);
}

IpcServerMetrics IpcServer::metrics_snapshot() const noexcept
{
    return impl_->metrics_snapshot();
}

} // namespace paperbreak::ipc
