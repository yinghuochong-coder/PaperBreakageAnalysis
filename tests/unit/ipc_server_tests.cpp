#include "paperbreak/ipc/server.hpp"

#include <QByteArray>
#include <QLocalSocket>
#include <QString>

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{

using Json = nlohmann::json;

class FixedAuthorizer final : public paperbreak::ipc::IPeerAuthorizer
{
  public:
    explicit FixedAuthorizer(const bool administrator = false) : administrator_(administrator) {}

    [[nodiscard]] paperbreak::Result<paperbreak::ipc::PeerIdentity> authorize(
        std::uintptr_t) noexcept override
    {
        return paperbreak::Result<paperbreak::ipc::PeerIdentity>::success(
            {.actor_sid = "S-1-5-21-test",
             .local = true,
             .authenticated = true,
             .administrator = administrator_});
    }

  private:
    bool administrator_{};
};

class RecordingNativeAuthorizer final : public paperbreak::ipc::IPeerAuthorizer
{
  public:
    explicit RecordingNativeAuthorizer(std::shared_ptr<std::optional<paperbreak::Error>> error)
        : inner_(paperbreak::ipc::make_windows_peer_authorizer()), error_(std::move(error))
    {
    }

    [[nodiscard]] paperbreak::Result<paperbreak::ipc::PeerIdentity> authorize(
        const std::uintptr_t descriptor) noexcept override
    {
        auto result = inner_->authorize(descriptor);
        if (!result)
        {
            *error_ = result.error();
        }
        return result;
    }

  private:
    std::unique_ptr<paperbreak::ipc::IPeerAuthorizer> inner_;
    std::shared_ptr<std::optional<paperbreak::Error>> error_;
};

class IdentityAuthorizer final : public paperbreak::ipc::IPeerAuthorizer
{
  public:
    explicit IdentityAuthorizer(paperbreak::ipc::PeerIdentity identity)
        : identity_(std::move(identity))
    {
    }

    [[nodiscard]] paperbreak::Result<paperbreak::ipc::PeerIdentity> authorize(
        std::uintptr_t) noexcept override
    {
        return paperbreak::Result<paperbreak::ipc::PeerIdentity>::success(identity_);
    }

  private:
    paperbreak::ipc::PeerIdentity identity_;
};

class FailingAuthorizer final : public paperbreak::ipc::IPeerAuthorizer
{
  public:
    [[nodiscard]] paperbreak::Result<paperbreak::ipc::PeerIdentity> authorize(
        std::uintptr_t) noexcept override
    {
        return paperbreak::Result<paperbreak::ipc::PeerIdentity>::failure(
            paperbreak::make_error("IPC_UNAUTHORIZED", paperbreak::Severity::warning,
                                   "authorization failed", "ipc", "ipc.test.authorize"));
    }
};

class EchoHandler final : public paperbreak::ipc::IRequestHandler
{
  public:
    [[nodiscard]] paperbreak::Result<paperbreak::ipc::CommandResponse> handle(
        const paperbreak::ipc::RequestMessage& request, const paperbreak::ipc::PeerIdentity&,
        std::stop_token) override
    {
        return paperbreak::Result<paperbreak::ipc::CommandResponse>::success(
            {.payload_json = request.payload_json, .binary = {}});
    }
};

class BlockingHandler final : public paperbreak::ipc::IRequestHandler
{
  public:
    [[nodiscard]] paperbreak::Result<paperbreak::ipc::CommandResponse> handle(
        const paperbreak::ipc::RequestMessage&, const paperbreak::ipc::PeerIdentity&,
        std::stop_token) override
    {
        std::unique_lock lock{mutex_};
        entered_ = true;
        condition_.notify_all();
        condition_.wait(lock, [this] { return released_; });
        return paperbreak::Result<paperbreak::ipc::CommandResponse>::success(
            {.payload_json = "{}", .binary = {}});
    }

    bool wait_until_entered()
    {
        std::unique_lock lock{mutex_};
        return condition_.wait_for(lock, std::chrono::seconds{2}, [this] { return entered_; });
    }

    void release()
    {
        {
            std::scoped_lock lock{mutex_};
            released_ = true;
        }
        condition_.notify_all();
    }

  private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool entered_{};
    bool released_{};
};

paperbreak::ipc::IpcServerOptions unique_options()
{
    static std::atomic_uint64_t sequence{0U};
    const std::string suffix = std::to_string(++sequence);
    paperbreak::ipc::IpcServerOptions options;
    options.server_name = "PaperBreakEdgeService.Ipc.Test." + suffix;
    options.instance_guard_name = L"Local\\PaperBreakEdgeService.Ipc.Test.Guard." +
                                  std::wstring{suffix.begin(), suffix.end()};
    options.startup_timeout = std::chrono::seconds{2};
    options.shutdown_flush_timeout = std::chrono::milliseconds{25};
    return options;
}

paperbreak::ipc::Frame request_frame(const std::string& request_id,
                                     const std::string& payload = "{}")
{
    Json header{{"protocolVersion", 1},
                {"messageType", "request"},
                {"requestId", request_id},
                {"command", "system.getStatus"},
                {"timestamp", "2026-08-01T12:00:00.123Z"},
                {"payload", Json::parse(payload)}};
    return {.header_json = header.dump(), .binary = {}};
}

bool connect_socket(QLocalSocket& socket, const std::string& name)
{
    socket.connectToServer(QString::fromStdString(name));
    return socket.waitForConnected(2000);
}

bool send_frame(QLocalSocket& socket, const paperbreak::ipc::Frame& frame)
{
    auto encoded = paperbreak::ipc::encode_frame(frame);
    if (!encoded)
    {
        return false;
    }
    const QByteArray bytes{reinterpret_cast<const char*>(encoded.value().data()),
                           static_cast<qsizetype>(encoded.value().size())};
    return socket.write(bytes) == bytes.size() && socket.waitForBytesWritten(2000);
}

QByteArray read_exact(QLocalSocket& socket, const qsizetype size)
{
    QByteArray result;
    while (result.size() < size)
    {
        if (socket.bytesAvailable() == 0 && !socket.waitForReadyRead(2000))
        {
            return {};
        }
        result += socket.read(size - result.size());
    }
    return result;
}

std::uint32_t read_u32(const char* bytes)
{
    const auto* data = reinterpret_cast<const unsigned char*>(bytes);
    return (static_cast<std::uint32_t>(data[0]) << 24U) |
           (static_cast<std::uint32_t>(data[1]) << 16U) |
           (static_cast<std::uint32_t>(data[2]) << 8U) | static_cast<std::uint32_t>(data[3]);
}

Json read_header(QLocalSocket& socket)
{
    const QByteArray prefix = read_exact(socket, 8);
    if (prefix.size() != 8)
    {
        return Json{};
    }
    const std::uint32_t header_size = read_u32(prefix.constData());
    const std::uint32_t binary_size = read_u32(prefix.constData() + 4);
    const QByteArray header = read_exact(socket, static_cast<qsizetype>(header_size));
    if (header.size() != static_cast<qsizetype>(header_size))
    {
        return Json{};
    }
    if (binary_size > 0U)
    {
        static_cast<void>(read_exact(socket, static_cast<qsizetype>(binary_size)));
    }
    return Json::parse(header.constData(), header.constData() + header.size());
}

void stop_server(paperbreak::ipc::IpcServer& server)
{
    server.request_stop();
    ASSERT_TRUE(server.join(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
}

bool wait_for_disconnect(QLocalSocket& socket)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (socket.state() != QLocalSocket::UnconnectedState &&
           std::chrono::steady_clock::now() < deadline)
    {
        static_cast<void>(socket.waitForDisconnected(50));
    }
    return socket.state() == QLocalSocket::UnconnectedState;
}

} // namespace

TEST(IpcServer, ServesRequestAndRejectsDuplicateRequestId)
{
    auto options = unique_options();
    auto handler = std::make_shared<EchoHandler>();
    paperbreak::ipc::IpcServer server{handler, std::make_unique<FixedAuthorizer>(), options};
    ASSERT_TRUE(server.start());
    QLocalSocket socket;
    ASSERT_TRUE(connect_socket(socket, options.server_name));

    const std::string request_id = "019870f2-6c80-7a31-9b52-6e3b9ca1d88f";
    ASSERT_TRUE(send_frame(socket, request_frame(request_id, R"({"value":7})")));
    const Json success = read_header(socket);
    ASSERT_TRUE(success.at("success").get<bool>());
    EXPECT_EQ(success.at("payload").at("value"), 7);

    ASSERT_TRUE(send_frame(socket, request_frame(request_id)));
    const Json duplicate = read_header(socket);
    EXPECT_FALSE(duplicate.at("success").get<bool>());
    EXPECT_EQ(duplicate.at("error").at("businessCode"), "IPC_REQUEST_CONFLICT");
    stop_server(server);
}

TEST(IpcServer, DisconnectsSlowIncompleteFrameOnAbsoluteDeadline)
{
    auto options = unique_options();
    options.incomplete_frame_timeout = std::chrono::milliseconds{50};
    paperbreak::ipc::IpcServer server{std::make_shared<EchoHandler>(),
                                      std::make_unique<FixedAuthorizer>(), options};
    ASSERT_TRUE(server.start());
    QLocalSocket socket;
    ASSERT_TRUE(connect_socket(socket, options.server_name));
    const QByteArray partial{"\0\0\0\x64\0\0\0\0{", 9};
    ASSERT_EQ(socket.write(partial), partial.size());
    ASSERT_TRUE(socket.waitForBytesWritten(1000));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (socket.state() != QLocalSocket::UnconnectedState &&
           std::chrono::steady_clock::now() < deadline)
    {
        static_cast<void>(socket.waitForDisconnected(100));
    }
    EXPECT_EQ(socket.state(), QLocalSocket::UnconnectedState);
    stop_server(server);
}

TEST(IpcServer, EnforcesCommandQueueCapacityWithoutBlockingEventThread)
{
    auto options = unique_options();
    options.command_queue_capacity = 1U;
    options.maximum_in_flight_per_connection = 3U;
    auto handler = std::make_shared<BlockingHandler>();
    paperbreak::ipc::IpcServer server{handler, std::make_unique<FixedAuthorizer>(), options};
    ASSERT_TRUE(server.start());
    QLocalSocket socket;
    ASSERT_TRUE(connect_socket(socket, options.server_name));

    ASSERT_TRUE(send_frame(socket, request_frame("019870f2-6c80-7a31-9b52-6e3b9ca1d801")));
    ASSERT_TRUE(handler->wait_until_entered());
    ASSERT_TRUE(send_frame(socket, request_frame("019870f2-6c80-7a31-9b52-6e3b9ca1d802")));
    ASSERT_TRUE(send_frame(socket, request_frame("019870f2-6c80-7a31-9b52-6e3b9ca1d803")));
    const Json busy = read_header(socket);
    EXPECT_EQ(busy.at("requestId"), "019870f2-6c80-7a31-9b52-6e3b9ca1d803");
    EXPECT_EQ(busy.at("error").at("businessCode"), "IPC_BUSY");

    handler->release();
    static_cast<void>(read_header(socket));
    static_cast<void>(read_header(socket));
    stop_server(server);
}

TEST(IpcServer, PublishesToConnectedClientsAndDoesNotReplayAfterDisconnect)
{
    auto options = unique_options();
    paperbreak::ipc::IpcServer server{std::make_shared<EchoHandler>(),
                                      std::make_unique<FixedAuthorizer>(), options};
    ASSERT_TRUE(server.start());
    QLocalSocket socket;
    ASSERT_TRUE(connect_socket(socket, options.server_name));
    ASSERT_TRUE(send_frame(socket, request_frame("019870f2-6c80-7a31-9b52-6e3b9ca1d811")));
    static_cast<void>(read_header(socket));

    ASSERT_TRUE(server.try_publish({.event_name = "status.changed",
                                    .timestamp = "2026-08-01T12:00:00.123Z",
                                    .payload_json = R"({"serviceState":"running"})",
                                    .binary = {},
                                    .coalescing_key = "status.changed"},
                                   paperbreak::ipc::PushPolicy::coalesce_latest));
    const Json push = read_header(socket);
    EXPECT_EQ(push.at("eventName"), "status.changed");
    socket.abort();

    EXPECT_TRUE(server.try_publish({.event_name = "status.changed",
                                    .timestamp = "2026-08-01T12:00:01.123Z",
                                    .payload_json = R"({"serviceState":"running"})",
                                    .binary = {},
                                    .coalescing_key = "status.changed"},
                                   paperbreak::ipc::PushPolicy::coalesce_latest));
    stop_server(server);
}

TEST(IpcServer, RejectsDuplicateServerGuardAndStopsWithActiveClient)
{
    auto options = unique_options();
    paperbreak::ipc::IpcServer first{std::make_shared<EchoHandler>(),
                                     std::make_unique<FixedAuthorizer>(), options};
    ASSERT_TRUE(first.start());
    auto second_options = options;
    second_options.server_name += ".second";
    paperbreak::ipc::IpcServer second{std::make_shared<EchoHandler>(),
                                      std::make_unique<FixedAuthorizer>(), second_options};
    auto duplicate = second.start();
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().business_code, "IPC_BUSY");

    QLocalSocket socket;
    ASSERT_TRUE(connect_socket(socket, options.server_name));
    stop_server(first);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (socket.state() != QLocalSocket::UnconnectedState &&
           std::chrono::steady_clock::now() < deadline)
    {
        static_cast<void>(socket.waitForDisconnected(50));
    }
    EXPECT_EQ(socket.state(), QLocalSocket::UnconnectedState);
}

TEST(IpcServer, RejectsConnectionsBeyondConfiguredActiveLimit)
{
    auto options = unique_options();
    options.maximum_connections = 2U;
    paperbreak::ipc::IpcServer server{std::make_shared<EchoHandler>(),
                                      std::make_unique<FixedAuthorizer>(), options};
    ASSERT_TRUE(server.start());
    QLocalSocket first;
    QLocalSocket second;
    QLocalSocket third;
    ASSERT_TRUE(connect_socket(first, options.server_name));
    ASSERT_TRUE(connect_socket(second, options.server_name));
    ASSERT_TRUE(connect_socket(third, options.server_name));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (third.state() != QLocalSocket::UnconnectedState &&
           std::chrono::steady_clock::now() < deadline)
    {
        static_cast<void>(third.waitForDisconnected(50));
    }
    EXPECT_EQ(third.state(), QLocalSocket::UnconnectedState);
    EXPECT_EQ(first.state(), QLocalSocket::ConnectedState);
    EXPECT_EQ(second.state(), QLocalSocket::ConnectedState);
    stop_server(server);
}

TEST(IpcServer, ReturnsUnsupportedVersionThenClosesConnection)
{
    auto options = unique_options();
    paperbreak::ipc::IpcServer server{std::make_shared<EchoHandler>(),
                                      std::make_unique<FixedAuthorizer>(), options};
    ASSERT_TRUE(server.start());
    QLocalSocket socket;
    ASSERT_TRUE(connect_socket(socket, options.server_name));
    auto frame = request_frame("019870f2-6c80-7a31-9b52-6e3b9ca1d831");
    Json header = Json::parse(frame.header_json);
    header["protocolVersion"] = 2;
    frame.header_json = header.dump();
    ASSERT_TRUE(send_frame(socket, frame));
    const Json response = read_header(socket);
    EXPECT_EQ(response.at("error").at("businessCode"), "IPC_PROTOCOL_VERSION_UNSUPPORTED");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (socket.state() != QLocalSocket::UnconnectedState &&
           std::chrono::steady_clock::now() < deadline)
    {
        static_cast<void>(socket.waitForDisconnected(50));
    }
    EXPECT_EQ(socket.state(), QLocalSocket::UnconnectedState);
    stop_server(server);
}

TEST(IpcServer, DisconnectsWhenResponseExceedsOutboundByteCapacity)
{
    auto options = unique_options();
    options.outbound_byte_capacity = 64U;
    paperbreak::ipc::IpcServer server{std::make_shared<EchoHandler>(),
                                      std::make_unique<FixedAuthorizer>(), options};
    ASSERT_TRUE(server.start());
    QLocalSocket socket;
    ASSERT_TRUE(connect_socket(socket, options.server_name));
    ASSERT_TRUE(send_frame(socket, request_frame("019870f2-6c80-7a31-9b52-6e3b9ca1d841")));
    EXPECT_TRUE(wait_for_disconnect(socket));
    stop_server(server);
}

TEST(IpcServer, DisconnectsRemoteAndAnonymousPeersBeforeDispatch)
{
    const std::vector identities{
        paperbreak::ipc::PeerIdentity{.actor_sid = "S-1-5-21-remote",
                                      .local = false,
                                      .authenticated = true,
                                      .administrator = false},
        paperbreak::ipc::PeerIdentity{
            .actor_sid = {}, .local = true, .authenticated = false, .administrator = false}};

    for (const auto& identity : identities)
    {
        auto options = unique_options();
        paperbreak::ipc::IpcServer server{std::make_shared<EchoHandler>(),
                                          std::make_unique<IdentityAuthorizer>(identity), options};
        ASSERT_TRUE(server.start());
        QLocalSocket socket;
        ASSERT_TRUE(connect_socket(socket, options.server_name));
        ASSERT_TRUE(send_frame(socket, request_frame("019870f2-6c80-7a31-9b52-6e3b9ca1d851")));
        EXPECT_TRUE(wait_for_disconnect(socket));
        stop_server(server);
    }
}

TEST(IpcServer, DisconnectsWhenPeerAuthorizationFails)
{
    auto options = unique_options();
    paperbreak::ipc::IpcServer server{std::make_shared<EchoHandler>(),
                                      std::make_unique<FailingAuthorizer>(), options};
    ASSERT_TRUE(server.start());
    QLocalSocket socket;
    ASSERT_TRUE(connect_socket(socket, options.server_name));
    ASSERT_TRUE(send_frame(socket, request_frame("019870f2-6c80-7a31-9b52-6e3b9ca1d852")));
    EXPECT_TRUE(wait_for_disconnect(socket));
    stop_server(server);
}

TEST(IpcServer, AuthorizesCurrentLocalNamedPipePeerWithWindowsToken)
{
    auto options = unique_options();
    auto authorization_error = std::make_shared<std::optional<paperbreak::Error>>();
    paperbreak::ipc::IpcServer server{
        std::make_shared<EchoHandler>(),
        std::make_unique<RecordingNativeAuthorizer>(authorization_error), options};
    ASSERT_TRUE(server.start());
    QLocalSocket socket;
    ASSERT_TRUE(connect_socket(socket, options.server_name));
    ASSERT_TRUE(send_frame(socket, request_frame("019870f2-6c80-7a31-9b52-6e3b9ca1d821")));
    const Json response = read_header(socket);
    ASSERT_FALSE(response.is_null())
        << (authorization_error->has_value()
                ? authorization_error->value().business_code + ":" +
                      authorization_error->value().native_code.value_or("none") + ":" +
                      authorization_error->value().message
                : "no authorization error");
    EXPECT_TRUE(response.at("success").get<bool>());
    stop_server(server);
}
