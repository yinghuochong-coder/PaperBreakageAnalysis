#pragma once

#include "paperbreak/camera/frame.hpp"
#include "paperbreak/common/result.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace paperbreak::camera
{

enum class ExposureAutoMode
{
    off,
    once,
    continuous,
};

enum class TriggerMode
{
    continuous,
    hardware,
    software,
};

enum class DigitalIoDirection
{
    input,
    output,
};

enum class CameraErrorKind
{
    not_found,
    open_failed,
    access_denied,
    config_failed,
    parameter_read_failed,
    parameter_write_failed,
    parameter_faulted,
    stream_start_failed,
    disconnected,
    frame_timeout,
    frame_incomplete,
    frame_format_changed,
    invalid_state_transition,
};

template <typename T> struct SteppedRange final
{
    T minimum{};
    T maximum{};
    T increment{};
    bool operator==(const SteppedRange&) const = default;
};

struct Roi final
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t offset_x{};
    std::uint32_t offset_y{};
    bool operator==(const Roi&) const = default;
};

struct RoiCapabilities final
{
    std::uint32_t sensor_width{};
    std::uint32_t sensor_height{};
    SteppedRange<std::uint32_t> width;
    SteppedRange<std::uint32_t> height;
    SteppedRange<std::uint32_t> offset_x;
    SteppedRange<std::uint32_t> offset_y;
    bool operator==(const RoiCapabilities&) const = default;
};

struct DigitalIoCapability final
{
    std::string line_id;
    DigitalIoDirection direction{DigitalIoDirection::input};
    bool writable{};
    bool operator==(const DigitalIoCapability&) const = default;
};

struct DigitalIoState final
{
    std::string line_id;
    bool value{};
    bool operator==(const DigitalIoState&) const = default;
};

struct LineIoCapabilities final
{
    bool alarm_input_supported{};
    bool line0_rising_edge_supported{};
    bool line0_falling_edge_supported{};
    bool strobe_output_supported{};
    std::optional<SteppedRange<std::uint32_t>> strobe_duration_us;
    std::optional<SteppedRange<std::uint32_t>> strobe_pre_delay_us;
    std::optional<SteppedRange<std::uint32_t>> strobe_post_delay_us;
    std::string unsupported_reason;
    bool operator==(const LineIoCapabilities&) const = default;
};

struct LineIoParameters final
{
    bool alarm_input_enabled{};
    bool strobe_output_enabled{};
    std::uint32_t strobe_duration_us{};
    std::uint32_t strobe_pre_delay_us{};
    std::uint32_t strobe_post_delay_us{};
    bool operator==(const LineIoParameters&) const = default;
};

struct LineInputState final
{
    bool enabled{};
    bool raw_level{};
    std::uint64_t revision{};
    std::int64_t timestamp_utc_ms{};
    bool operator==(const LineInputState&) const = default;
};

struct LineInputEvent final
{
    bool raw_level{};
    std::uint64_t revision{};
    std::int64_t timestamp_utc_ms{};
    bool operator==(const LineInputEvent&) const = default;
};

using LineInputObserver = std::function<void(const LineInputEvent&)>;

struct CameraDeviceDescriptor final
{
    std::string model_name;
    std::string serial_number;
    std::string ip_address;
    std::string network_interface;
    bool exclusive_access_available{true};
    bool operator==(const CameraDeviceDescriptor&) const = default;
};

enum class CameraSlotStatus
{
    ready,
    missing,
    occupied,
};

struct CameraSlotBinding final
{
    std::string camera_id;
    std::string serial_number;
    bool operator==(const CameraSlotBinding&) const = default;
};

struct CameraSlotDiscovery final
{
    std::string camera_id;
    std::string serial_number;
    CameraSlotStatus status{CameraSlotStatus::missing};
    std::optional<CameraDeviceDescriptor> device;
    bool operator==(const CameraSlotDiscovery&) const = default;
};

struct CameraDiscoveryReport final
{
    std::vector<CameraSlotDiscovery> slots;
    std::vector<CameraDeviceDescriptor> unexpected_devices;
    bool operator==(const CameraDiscoveryReport&) const = default;
};

struct CameraCapabilities final
{
    std::optional<SteppedRange<double>> exposure_us;
    std::vector<ExposureAutoMode> exposure_auto_modes;
    std::optional<SteppedRange<double>> gain_db;
    std::optional<SteppedRange<double>> frame_rate;
    std::optional<RoiCapabilities> roi;
    bool supports_reverse_x{};
    bool supports_reverse_y{};
    std::vector<PixelFormat> pixel_formats;
    std::vector<TriggerMode> trigger_modes;
    std::vector<std::string> trigger_sources;
    std::optional<SteppedRange<std::uint32_t>> trigger_delay_us;
    std::optional<SteppedRange<std::uint32_t>> packet_size_bytes;
    std::optional<SteppedRange<std::uint32_t>> inter_packet_delay_ns;
    std::vector<DigitalIoCapability> digital_io;
    LineIoCapabilities line_io;
    bool supports_user_sets{};
    bool supports_restore_defaults{};
    std::size_t maximum_payload_bytes{};
    bool operator==(const CameraCapabilities&) const = default;
};

struct CameraParameterSnapshot final
{
    std::optional<double> exposure_us;
    std::optional<ExposureAutoMode> exposure_auto_mode;
    std::optional<double> gain_db;
    std::optional<double> frame_rate;
    std::optional<Roi> roi;
    std::optional<bool> reverse_x;
    std::optional<bool> reverse_y;
    std::optional<PixelFormat> pixel_format;
    std::optional<TriggerMode> trigger_mode;
    std::optional<std::string> trigger_source;
    std::optional<std::uint32_t> trigger_delay_us;
    std::optional<std::uint32_t> packet_size_bytes;
    std::optional<std::uint32_t> inter_packet_delay_ns;
    std::vector<DigitalIoState> digital_io;
    std::optional<LineIoParameters> line_io;
    std::optional<LineInputState> line_input;
    bool operator==(const CameraParameterSnapshot&) const = default;
};

struct CapturedFrameMetadata final
{
    std::uint64_t camera_frame_number{};
    std::optional<CameraTimestamp> camera_timestamp;
    FrameGeometry geometry;
    PixelFormat pixel_format{PixelFormat::mono8};
    FrameFlags flags;
    bool operator==(const CapturedFrameMetadata&) const = default;
};

class ICameraDevice
{
  public:
    virtual ~ICameraDevice() = default;

    [[nodiscard]] virtual const CameraDeviceDescriptor& descriptor() const noexcept = 0;
    [[nodiscard]] virtual Result<void> connect() = 0;
    [[nodiscard]] virtual Result<void> disconnect() = 0;
    virtual void set_line_input_observer(LineInputObserver) {}
    [[nodiscard]] virtual Result<CameraCapabilities> capabilities() = 0;
    [[nodiscard]] virtual Result<CameraParameterSnapshot> read_parameters() = 0;
    [[nodiscard]] virtual Result<CameraParameterSnapshot> apply_parameters(
        const CameraParameterSnapshot& parameters) = 0;
    [[nodiscard]] virtual Result<void> start_acquisition() = 0;
    [[nodiscard]] virtual Result<CapturedFrameMetadata> capture_into(
        FrameBuffer& destination, std::chrono::milliseconds timeout) = 0;
    [[nodiscard]] virtual Result<void> software_trigger() = 0;
    [[nodiscard]] virtual Result<void> stop_acquisition() = 0;
    [[nodiscard]] virtual Result<void> save_user_set(std::string_view name) = 0;
    [[nodiscard]] virtual Result<CameraParameterSnapshot> restore_defaults() = 0;
};

class ICameraProvider
{
  public:
    virtual ~ICameraProvider() = default;

    [[nodiscard]] virtual Result<std::vector<CameraDeviceDescriptor>> enumerate_devices() = 0;
    [[nodiscard]] virtual Result<std::unique_ptr<ICameraDevice>> create_device(
        std::string_view serial_number) = 0;
};

[[nodiscard]] std::string_view camera_business_code(CameraErrorKind kind) noexcept;
[[nodiscard]] Error make_camera_error(CameraErrorKind kind, std::string message,
                                      std::string operation,
                                      std::optional<std::string> source_id = std::nullopt,
                                      std::vector<ErrorDetail> details = {});

[[nodiscard]] Result<void> validate_device_inventory(
    std::span<const CameraDeviceDescriptor> devices);
[[nodiscard]] Result<CameraDeviceDescriptor> find_device_by_serial(
    std::span<const CameraDeviceDescriptor> devices, std::string_view serial_number);
[[nodiscard]] Result<CameraDiscoveryReport> reconcile_camera_slots(
    std::span<const CameraSlotBinding> bindings, std::span<const CameraDeviceDescriptor> devices);
[[nodiscard]] Result<void> validate_parameters(const CameraCapabilities& capabilities,
                                               const CameraParameterSnapshot& parameters);
[[nodiscard]] Result<CameraParameterSnapshot> apply_validated_parameters(
    ICameraDevice& device, const CameraParameterSnapshot& parameters);

} // namespace paperbreak::camera
