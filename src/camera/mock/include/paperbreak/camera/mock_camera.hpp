#pragma once

#include "paperbreak/camera/camera.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace paperbreak::camera::mock
{

namespace detail
{
struct MockSharedState;
}

inline constexpr std::size_t maximum_mock_camera_count = 4U;
inline constexpr std::size_t maximum_mock_control_capacity = 1024U;

/// Deterministic image generator selected independently for each mock camera.
enum class MockFramePattern
{
    gradient,
    checkerboard,
    noise,
};

/// One-shot device behaviors available to scripts and the runtime control API.
enum class MockFaultKind
{
    disconnect,
    timeout,
    drop_frame,
    incomplete_frame,
    frame_number_jump,
    geometry_change,
    pixel_format_change,
};

/// Fault payload. Only jump/drop, geometry, and pixel-format faults consume extra fields.
struct MockFault final
{
    MockFaultKind kind{MockFaultKind::timeout};
    std::uint64_t amount{1U};
    std::optional<FrameGeometry> geometry;
    std::optional<PixelFormat> pixel_format;
    bool operator==(const MockFault&) const = default;
};

/// Immutable one-shot fault associated with a one-based acquisition opportunity.
struct MockScheduledFault final
{
    /// One-based acquisition opportunity. Trigger waits that time out do not advance it.
    std::uint64_t capture_attempt{};
    MockFault fault;
    bool operator==(const MockScheduledFault&) const = default;
};

/// Fully in-memory device definition used by MockCameraProvider::create.
struct MockCameraConfig final
{
    CameraDeviceDescriptor descriptor;
    std::uint32_t width{64U};
    std::uint32_t height{48U};
    double frame_rate{30.0};
    PixelFormat pixel_format{PixelFormat::mono8};
    TriggerMode trigger_mode{TriggerMode::continuous};
    MockFramePattern pattern{MockFramePattern::gradient};
    std::uint64_t random_seed{1U};
    /// Declared upper bound for normal and injected frame payloads. Zero means base-frame size.
    std::size_t maximum_payload_bytes{};
    std::size_t fault_queue_capacity{16U};
    std::size_t trigger_capacity{16U};
    LineIoCapabilities line_io_capabilities{
        .alarm_input_supported = true,
        .line0_rising_edge_supported = true,
        .line0_falling_edge_supported = true,
        .strobe_output_supported = true,
        .strobe_duration_us = SteppedRange<std::uint32_t>{1U, 1000000U, 1U},
        .strobe_pre_delay_us = SteppedRange<std::uint32_t>{0U, 1000000U, 1U},
        .strobe_post_delay_us = SteppedRange<std::uint32_t>{0U, 1000000U, 1U}};
    bool initial_line0_level{};
    std::vector<MockScheduledFault> fault_script;
    bool operator==(const MockCameraConfig&) const = default;
};

/// Bounded diagnostic snapshot; M2-05 production acquisition statistics are separate.
struct MockCameraControlSnapshot final
{
    bool connected{};
    bool streaming{};
    std::uint64_t capture_attempts{};
    std::uint64_t frames_generated{};
    std::uint64_t camera_frame_number{};
    std::size_t queued_faults{};
    std::uint64_t faults_accepted{};
    std::uint64_t faults_rejected{};
    std::uint64_t faults_executed{};
    std::size_t pending_software_triggers{};
    std::size_t pending_hardware_triggers{};
    std::uint64_t triggers_rejected{};
};

/// Thread-safe test handle for fault injection and simulated hardware edges.
class MockCameraControl final
{
  public:
    MockCameraControl() = default;

    /// Enqueues a one-shot fault for the next acquisition opportunity.
    [[nodiscard]] Result<void> inject_fault(MockFault fault) const;
    /// Enqueues simulated hardware edges. Only valid in hardware-trigger mode.
    [[nodiscard]] Result<void> hardware_trigger(std::size_t count = 1U) const;
    /// Changes Line 0 and emits a device input event when the input is enabled and connected.
    [[nodiscard]] Result<void> set_line_input(bool raw_level) const;
    /// Clears queued runtime faults without rewinding the immutable startup script.
    void clear_faults() const noexcept;
    /// Returns a consistent state and bounded-channel snapshot.
    [[nodiscard]] Result<MockCameraControlSnapshot> snapshot() const;

  private:
    friend class MockCameraProvider;
    explicit MockCameraControl(std::weak_ptr<detail::MockSharedState> state);

    std::weak_ptr<detail::MockSharedState> state_;
};

/// Vendor-independent provider for one to four deterministic in-memory cameras.
class MockCameraProvider final : public ICameraProvider
{
  private:
    struct ValidatedTag final
    {
    };

  public:
    /// Validates all configurations and constructs a provider with one to four devices.
    [[nodiscard]] static Result<std::unique_ptr<MockCameraProvider>> create(
        std::vector<MockCameraConfig> configurations);

    ~MockCameraProvider() override;
    MockCameraProvider(const MockCameraProvider&) = delete;
    MockCameraProvider& operator=(const MockCameraProvider&) = delete;

    [[nodiscard]] Result<std::vector<CameraDeviceDescriptor>> enumerate_devices() override;
    [[nodiscard]] Result<std::unique_ptr<ICameraDevice>> create_device(
        std::string_view serial_number) override;
    /// Returns a test control handle for one configured serial number.
    [[nodiscard]] Result<MockCameraControl> control(std::string_view serial_number) const;

    MockCameraProvider(ValidatedTag, std::vector<std::shared_ptr<detail::MockSharedState>> states);

  private:
    std::vector<std::shared_ptr<detail::MockSharedState>> states_;
};

} // namespace paperbreak::camera::mock
