#pragma once

#include "paperbreak/uplink/transport.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace paperbreak::uplink::mock
{

inline constexpr std::size_t maximum_mock_fault_capacity = 1024U;
inline constexpr std::size_t maximum_mock_history_capacity = 4096U;
inline constexpr auto maximum_mock_delay = std::chrono::seconds{10};

enum class MockFaultKind
{
    offline,
    slow_response,
    retryable_failure,
    permanent_failure,
    duplicate_acknowledgement,
    checksum_mismatch,
};

struct MockFaultStep final
{
    MockFaultKind kind{MockFaultKind::offline};
    std::optional<UplinkOperation> operation;
    std::chrono::milliseconds delay{};
};

struct MockUplinkConfig final
{
    std::size_t fault_capacity{64U};
    std::size_t history_capacity{256U};
    std::chrono::milliseconds maximum_delay{std::chrono::seconds{5}};
    std::uint32_t heartbeat_seconds{5U};
};

struct MockCallRecord final
{
    UplinkOperation operation{UplinkOperation::connect};
    std::string correlation_id;
    bool succeeded{};
    std::string business_code;
    std::uint32_t acknowledgement_delivery_count{};
};

struct MockUplinkSnapshot final
{
    bool online{};
    UplinkConnectionState connection_state{UplinkConnectionState::disconnected};
    std::size_t queued_faults{};
    std::size_t retained_history{};
    std::uint64_t calls{};
    std::uint64_t successes{};
    std::uint64_t failures{};
    std::uint64_t duplicate_acknowledgements{};
    std::uint64_t checksum_mismatches{};
    std::uint64_t rejected_faults{};
};

namespace detail
{
struct MockUplinkState;
}

class MockUplinkTransport final : public IUplinkTransport
{
  private:
    struct ValidatedTag final
    {
    };

  public:
    ~MockUplinkTransport() override;

    MockUplinkTransport(const MockUplinkTransport&) = delete;
    MockUplinkTransport& operator=(const MockUplinkTransport&) = delete;

    [[nodiscard]] static Result<std::unique_ptr<MockUplinkTransport>> create(
        MockUplinkConfig config = {});

    /// Public only for std::make_unique; ValidatedTag is private so callers must use create().
    MockUplinkTransport(ValidatedTag, MockUplinkConfig config);

    [[nodiscard]] Result<TransportSession> connect(const SessionHello& hello) override;
    void disconnect() noexcept override;
    [[nodiscard]] UplinkConnectionState connection_state() const noexcept override;
    [[nodiscard]] Result<TransportAcknowledgement> send_heartbeat(
        const MessageEnvelope& heartbeat) override;
    [[nodiscard]] Result<TransportAcknowledgement> send_control_message(
        const MessageEnvelope& message) override;
    [[nodiscard]] Result<TransportAcknowledgement> send_event_metadata(
        const EventMetadataRequest& event) override;
    [[nodiscard]] Result<TransportAcknowledgement> upload_file(
        const UploadFileRequest& request) override;
    void set_command_handler(CommandHandler handler) override;

    [[nodiscard]] Result<void> enqueue_fault(MockFaultStep fault);
    void clear_faults() noexcept;
    void set_online(bool online) noexcept;
    [[nodiscard]] Result<void> inject_command(const MessageEnvelope& command);
    [[nodiscard]] MockUplinkSnapshot snapshot() const noexcept;
    [[nodiscard]] std::vector<MockCallRecord> history() const;
    void clear_history() noexcept;

  private:
    std::shared_ptr<detail::MockUplinkState> state_;
};

} // namespace paperbreak::uplink::mock
