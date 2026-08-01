#include "paperbreak/ipc/client.hpp"
#include "paperbreak/ipc/server.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QLocalServer>
#include <QLocalSocket>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{

class LocalAuthorizer final : public paperbreak::ipc::IPeerAuthorizer
{
  public:
    [[nodiscard]] paperbreak::Result<paperbreak::ipc::PeerIdentity> authorize(
        std::uintptr_t) noexcept override
    {
        return paperbreak::Result<paperbreak::ipc::PeerIdentity>::success(
            {.actor_sid = "S-1-5-21-client-test",
             .local = true,
             .authenticated = true,
             .administrator = false});
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
            {.payload_json = request.payload_json, .binary = request.binary});
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

    [[nodiscard]] bool entered() const
    {
        std::scoped_lock lock{mutex_};
        return entered_;
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
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool entered_{};
    bool released_{};
};

std::string unique_name()
{
    static std::atomic_uint64_t sequence{};
    return "PaperBreakEdgeService.Ipc.ClientTest." + std::to_string(++sequence);
}

paperbreak::ipc::IpcServerOptions server_options(const std::string& name)
{
    paperbreak::ipc::IpcServerOptions options;
    options.server_name = name;
    const std::wstring suffix{name.begin(), name.end()};
    options.instance_guard_name = L"Local\\" + suffix + L".Guard";
    options.startup_timeout = std::chrono::seconds{2};
    options.shutdown_flush_timeout = std::chrono::milliseconds{10};
    return options;
}

paperbreak::ipc::IpcClientOptions client_options(const std::string& name)
{
    paperbreak::ipc::IpcClientOptions options;
    options.server_name = name;
    options.connect_timeout = std::chrono::milliseconds{50};
    options.default_request_timeout = std::chrono::milliseconds{500};
    options.initial_reconnect_delay = std::chrono::milliseconds{10};
    options.maximum_reconnect_delay = std::chrono::milliseconds{40};
    options.stable_connection_reset = std::chrono::milliseconds{100};
    options.reconnect_jitter_fraction = 0.0;
    return options;
}

bool wait_until(const std::function<bool()>& predicate,
                const std::chrono::milliseconds timeout = std::chrono::seconds{2})
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (predicate())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}

void stop_server(paperbreak::ipc::IpcServer& server)
{
    server.request_stop();
    ASSERT_TRUE(server.join(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
}

paperbreak::ipc::Frame read_frame(QLocalSocket& socket)
{
    paperbreak::ipc::FrameDecoder decoder;
    std::optional<paperbreak::ipc::Frame> frame;
    static_cast<void>(wait_until([&] {
        if (socket.bytesAvailable() == 0)
        {
            return false;
        }
        const QByteArray bytes = socket.readAll();
        const auto* data = reinterpret_cast<const std::byte*>(bytes.constData());
        auto decoded = decoder.append(
            std::span<const std::byte>{data, static_cast<std::size_t>(bytes.size())});
        if (decoded && !decoded.value().empty())
        {
            frame = std::move(decoded.value().front());
            return true;
        }
        return false;
    }));
    return frame.value_or(paperbreak::ipc::Frame{});
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
    const bool accepted = socket.write(bytes) == bytes.size();
    static_cast<void>(socket.flush());
    return accepted;
}

} // namespace

TEST(IpcClient, ConnectsToRunningServerAndCompletesRequest)
{
    const std::string name = unique_name();
    paperbreak::ipc::IpcServer server(std::make_shared<EchoHandler>(),
                                      std::make_unique<LocalAuthorizer>(), server_options(name));
    ASSERT_TRUE(server.start());

    paperbreak::ipc::ClientConnectionSnapshot latest;
    paperbreak::ipc::IpcClient client(
        {.connection_changed = [&](const auto& state) { latest = state; }}, client_options(name));
    ASSERT_TRUE(client.start());
    ASSERT_TRUE(wait_until(
        [&] { return latest.state == paperbreak::ipc::ClientConnectionState::connected; }));

    std::optional<paperbreak::Result<paperbreak::ipc::ResponseMessage>> completion;
    auto request =
        client.send_request("system.getStatus", R"({"probe":true})", {std::byte{0x42}},
                            [&](paperbreak::ipc::ClientRequestHandle,
                                paperbreak::Result<paperbreak::ipc::ResponseMessage> result) {
                                completion = std::move(result);
                            });
    ASSERT_TRUE(request);
    ASSERT_TRUE(wait_until([&] { return completion.has_value(); }));
    ASSERT_TRUE(completion.value());
    EXPECT_EQ(completion->value().binary, std::vector<std::byte>{std::byte{0x42}});

    client.stop();
    stop_server(server);
}

TEST(IpcClient, ConnectsAfterServerStartsAndReconnectsAfterRestart)
{
    const std::string name = unique_name();
    std::vector<paperbreak::ipc::ClientConnectionSnapshot> states;
    paperbreak::ipc::IpcClient client(
        {.connection_changed = [&](const auto& state) { states.push_back(state); }},
        client_options(name));
    ASSERT_TRUE(client.start());
    ASSERT_TRUE(wait_until([&] {
        return !states.empty() &&
               states.back().state == paperbreak::ipc::ClientConnectionState::retry_wait;
    }));

    auto first = std::make_unique<paperbreak::ipc::IpcServer>(
        std::make_shared<EchoHandler>(), std::make_unique<LocalAuthorizer>(), server_options(name));
    ASSERT_TRUE(first->start());
    ASSERT_TRUE(wait_until([&] {
        return !states.empty() &&
               states.back().state == paperbreak::ipc::ClientConnectionState::connected;
    }));
    const std::uint64_t first_generation = states.back().generation;

    stop_server(*first);
    first.reset();
    ASSERT_TRUE(wait_until([&] {
        return !states.empty() &&
               states.back().state == paperbreak::ipc::ClientConnectionState::retry_wait;
    }));
    auto second = std::make_unique<paperbreak::ipc::IpcServer>(
        std::make_shared<EchoHandler>(), std::make_unique<LocalAuthorizer>(), server_options(name));
    ASSERT_TRUE(second->start());
    ASSERT_TRUE(wait_until([&] {
        return !states.empty() &&
               states.back().state == paperbreak::ipc::ClientConnectionState::connected &&
               states.back().generation > first_generation;
    }));

    client.stop();
    stop_server(*second);
}

TEST(IpcClient, AppliesBoundedExponentialReconnectDelay)
{
    const std::string name = unique_name();
    auto options = client_options(name);
    std::vector<std::chrono::milliseconds> delays;
    paperbreak::ipc::IpcClient client(
        {.connection_changed =
             [&](const auto& state) {
                 if (state.state == paperbreak::ipc::ClientConnectionState::retry_wait &&
                     state.next_retry_delay.has_value())
                 {
                     delays.push_back(state.next_retry_delay.value());
                 }
             }},
        options);
    ASSERT_TRUE(client.start());
    ASSERT_TRUE(wait_until([&] { return delays.size() >= 4U; }));
    EXPECT_EQ(delays[0], std::chrono::milliseconds{10});
    EXPECT_EQ(delays[1], std::chrono::milliseconds{20});
    EXPECT_EQ(delays[2], std::chrono::milliseconds{40});
    EXPECT_EQ(delays[3], std::chrono::milliseconds{40});
    client.stop();
}

TEST(IpcClient, KeepsJitteredReconnectDelayWithinConfiguredCap)
{
    const std::string name = unique_name();
    auto options = client_options(name);
    options.initial_reconnect_delay = std::chrono::milliseconds{100};
    options.maximum_reconnect_delay = std::chrono::milliseconds{100};
    options.reconnect_jitter_fraction = 0.20;
    std::vector<std::chrono::milliseconds> delays;
    paperbreak::ipc::IpcClient client(
        {.connection_changed =
             [&](const auto& state) {
                 if (state.state == paperbreak::ipc::ClientConnectionState::retry_wait &&
                     state.next_retry_delay.has_value())
                 {
                     delays.push_back(state.next_retry_delay.value());
                 }
             }},
        options);
    ASSERT_TRUE(client.start());
    ASSERT_TRUE(wait_until([&] { return delays.size() >= 3U; }));
    for (const auto delay : delays)
    {
        EXPECT_GE(delay, std::chrono::milliseconds{80});
        EXPECT_LE(delay, std::chrono::milliseconds{100});
    }
    client.stop();
}

TEST(IpcClient, RejectsDisconnectedAndOversizedOutboundRequests)
{
    const std::string name = unique_name();
    auto options = client_options(name);
    options.outbound_byte_capacity = 512U;
    paperbreak::ipc::IpcClient client({}, options);
    ASSERT_TRUE(client.start());
    auto disconnected =
        client.send_request("system.getStatus", "{}", {},
                            [](paperbreak::ipc::ClientRequestHandle,
                               paperbreak::Result<paperbreak::ipc::ResponseMessage>) {});
    ASSERT_FALSE(disconnected);
    EXPECT_EQ(disconnected.error().business_code, "IPC_NOT_CONNECTED");
    client.stop();

    paperbreak::ipc::IpcServer server(std::make_shared<EchoHandler>(),
                                      std::make_unique<LocalAuthorizer>(), server_options(name));
    ASSERT_TRUE(server.start());
    paperbreak::ipc::IpcClient connected({}, options);
    ASSERT_TRUE(connected.start());
    ASSERT_TRUE(wait_until([&] {
        return connected.snapshot().state == paperbreak::ipc::ClientConnectionState::connected;
    }));
    auto too_large = connected.send_request(
        "system.getStatus", "{\"padding\":\"" + std::string(1024U, 'x') + "\"}", {},
        [](paperbreak::ipc::ClientRequestHandle,
           paperbreak::Result<paperbreak::ipc::ResponseMessage>) {});
    ASSERT_FALSE(too_large);
    EXPECT_EQ(too_large.error().business_code, "IPC_BUSY");
    connected.stop();
    stop_server(server);
}

TEST(IpcClient, EnforcesCapacityTimeoutCancellationAndDisconnectCompletion)
{
    const std::string name = unique_name();
    auto handler = std::make_shared<BlockingHandler>();
    paperbreak::ipc::IpcServer server(handler, std::make_unique<LocalAuthorizer>(),
                                      server_options(name));
    ASSERT_TRUE(server.start());
    auto options = client_options(name);
    options.maximum_pending_requests = 2U;
    paperbreak::ipc::IpcClient client({}, options);
    ASSERT_TRUE(client.start());
    ASSERT_TRUE(wait_until([&] {
        return client.snapshot().state == paperbreak::ipc::ClientConnectionState::connected;
    }));

    std::vector<std::string> errors;
    const auto callback = [&](paperbreak::ipc::ClientRequestHandle,
                              paperbreak::Result<paperbreak::ipc::ResponseMessage> result) {
        if (!result)
        {
            errors.push_back(result.error().business_code);
        }
    };
    auto first =
        client.send_request("system.getStatus", "{}", {}, callback, std::chrono::milliseconds{50});
    ASSERT_TRUE(first);
    ASSERT_TRUE(wait_until([&] { return handler->entered(); }));
    auto second = client.send_request("system.getStatus", "{}", {}, callback);
    ASSERT_TRUE(second);
    auto full = client.send_request("system.getStatus", "{}", {}, callback);
    ASSERT_FALSE(full);
    EXPECT_EQ(full.error().business_code, "IPC_BUSY");
    EXPECT_TRUE(client.cancel_request(second.value()));
    ASSERT_TRUE(wait_until([&] {
        return std::ranges::find(errors, "IPC_REQUEST_CANCELLED") != errors.end() &&
               std::ranges::find(errors, "IPC_REQUEST_TIMEOUT") != errors.end();
    }));

    handler->release();
    client.stop();
    stop_server(server);
}

TEST(IpcClient, IgnoresOldResponseOnNewConnectionAndRecoversFromMalformedMessage)
{
    const std::string name = unique_name();
    QLocalServer::removeServer(QString::fromStdString(name));
    QLocalServer raw_server;
    ASSERT_TRUE(raw_server.listen(QString::fromStdString(name)));
    paperbreak::ipc::IpcClient client({}, client_options(name));
    ASSERT_TRUE(client.start());
    ASSERT_TRUE(wait_until([&] { return raw_server.hasPendingConnections(); }));
    std::unique_ptr<QLocalSocket> first_socket{raw_server.nextPendingConnection()};
    ASSERT_TRUE(wait_until([&] {
        return client.snapshot().state == paperbreak::ipc::ClientConnectionState::connected;
    }));

    std::vector<std::string> old_errors;
    auto old_request =
        client.send_request("system.getStatus", "{}", {},
                            [&](paperbreak::ipc::ClientRequestHandle,
                                paperbreak::Result<paperbreak::ipc::ResponseMessage> result) {
                                if (!result)
                                {
                                    old_errors.push_back(result.error().business_code);
                                }
                            });
    ASSERT_TRUE(old_request);
    ASSERT_FALSE(read_frame(*first_socket).header_json.empty());
    first_socket->abort();
    first_socket.reset();
    ASSERT_TRUE(wait_until(
        [&] { return !old_errors.empty() && old_errors.front() == "IPC_CONNECTION_LOST"; }));
    ASSERT_TRUE(wait_until([&] { return raw_server.hasPendingConnections(); }));
    std::unique_ptr<QLocalSocket> second_socket{raw_server.nextPendingConnection()};
    ASSERT_TRUE(wait_until([&] {
        return client.snapshot().state == paperbreak::ipc::ClientConnectionState::connected &&
               client.snapshot().generation > old_request.value().generation;
    }));

    std::size_t new_completions{};
    auto new_request = client.send_request(
        "system.getStatus", "{}", {},
        [&](paperbreak::ipc::ClientRequestHandle,
            paperbreak::Result<paperbreak::ipc::ResponseMessage>) { ++new_completions; });
    ASSERT_TRUE(new_request);
    ASSERT_FALSE(read_frame(*second_socket).header_json.empty());
    auto old_response =
        paperbreak::ipc::encode_response({.request_id = old_request.value().request_id,
                                          .success = true,
                                          .timestamp = "2026-08-01T12:00:00.123Z",
                                          .payload_json = "{}"});
    ASSERT_TRUE(old_response);
    ASSERT_TRUE(send_frame(*second_socket, old_response.value()));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    EXPECT_EQ(new_completions, 0U);

    ASSERT_TRUE(
        send_frame(*second_socket, {.header_json = R"({"messageType":"invalid"})", .binary = {}}));
    ASSERT_TRUE(wait_until([&] {
        return client.snapshot().state == paperbreak::ipc::ClientConnectionState::retry_wait;
    }));
    EXPECT_EQ(new_completions, 1U);
    client.stop();
    raw_server.close();
    QLocalServer::removeServer(QString::fromStdString(name));
}

TEST(IpcClient, StoppingClientLeavesServerAvailable)
{
    const std::string name = unique_name();
    paperbreak::ipc::IpcServer server(std::make_shared<EchoHandler>(),
                                      std::make_unique<LocalAuthorizer>(), server_options(name));
    ASSERT_TRUE(server.start());
    {
        paperbreak::ipc::IpcClient first({}, client_options(name));
        ASSERT_TRUE(first.start());
        ASSERT_TRUE(first.start());
        ASSERT_TRUE(wait_until([&] {
            return first.snapshot().state == paperbreak::ipc::ClientConnectionState::connected;
        }));
        first.stop();
        first.stop();
    }
    paperbreak::ipc::IpcClient second({}, client_options(name));
    ASSERT_TRUE(second.start());
    ASSERT_TRUE(wait_until([&] {
        return second.snapshot().state == paperbreak::ipc::ClientConnectionState::connected;
    }));
    std::size_t completions{};
    ASSERT_TRUE(second.send_request(
        "system.getStatus", "{}", {},
        [&](paperbreak::ipc::ClientRequestHandle,
            paperbreak::Result<paperbreak::ipc::ResponseMessage>) { ++completions; }));
    EXPECT_TRUE(wait_until([&] { return completions == 1U; }));
    second.stop();
    stop_server(server);
}
