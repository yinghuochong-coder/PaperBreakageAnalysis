#include "paperbreak/camera/mock_camera.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace paperbreak::camera::mock
{
namespace
{
using Clock = std::chrono::steady_clock;
constexpr std::size_t maximum_mock_payload_bytes = 1024U * 1024U * 1024U;

Error mock_error(const CameraErrorKind kind, std::string message, std::string operation,
                 const std::string& serial_number, std::string reason)
{
    return make_camera_error(kind, std::move(message), std::move(operation), serial_number,
                             {{"reason", std::move(reason)}});
}

Error invalid_config(const std::string& serial_number, std::string reason)
{
    return mock_error(CameraErrorKind::config_failed, "模拟相机配置无效", "camera.mock.validate",
                      serial_number, std::move(reason));
}

Error invalid_state(const std::string& serial_number, std::string operation, std::string reason)
{
    return mock_error(CameraErrorKind::invalid_state_transition, "模拟相机状态不允许该操作",
                      std::move(operation), serial_number, std::move(reason));
}

std::size_t bytes_per_pixel(const PixelFormat format) noexcept
{
    switch (format)
    {
    case PixelFormat::mono8:
    case PixelFormat::bayer_rg8:
        return 1U;
    case PixelFormat::mono10:
    case PixelFormat::mono12:
        return 2U;
    }
    return 0U;
}

std::optional<FrameGeometry> make_geometry(const std::uint32_t width, const std::uint32_t height,
                                           const PixelFormat format) noexcept
{
    const auto pixel_bytes = bytes_per_pixel(format);
    if (width == 0U || height == 0U || pixel_bytes == 0U ||
        width > std::numeric_limits<std::uint32_t>::max() / pixel_bytes)
    {
        return std::nullopt;
    }
    return FrameGeometry{width, height, static_cast<std::uint32_t>(width * pixel_bytes)};
}

std::optional<std::size_t> payload_size(const FrameGeometry geometry) noexcept
{
    if (geometry.width == 0U || geometry.height == 0U || geometry.stride == 0U ||
        geometry.stride < geometry.width)
    {
        return std::nullopt;
    }
    if (geometry.height > std::numeric_limits<std::size_t>::max() / geometry.stride)
    {
        return std::nullopt;
    }
    return static_cast<std::size_t>(geometry.height) * geometry.stride;
}

bool valid_layout_for_format(const FrameGeometry geometry, const PixelFormat format) noexcept
{
    const auto pixel_bytes = bytes_per_pixel(format);
    return pixel_bytes > 0U &&
           geometry.width <= std::numeric_limits<std::uint32_t>::max() / pixel_bytes &&
           geometry.stride >= geometry.width * pixel_bytes;
}

Result<void> validate_fault(const MockFault& fault, const std::size_t maximum_payload,
                            const std::string& serial_number)
{
    switch (fault.kind)
    {
    case MockFaultKind::drop_frame:
    case MockFaultKind::frame_number_jump:
        if (fault.amount == 0U)
        {
            return Result<void>::failure(invalid_config(serial_number, "fault-amount-zero"));
        }
        break;
    case MockFaultKind::geometry_change:
        if (!fault.geometry)
        {
            return Result<void>::failure(invalid_config(serial_number, "missing-fault-geometry"));
        }
        if (const auto size = payload_size(*fault.geometry); !size || *size > maximum_payload)
        {
            return Result<void>::failure(
                invalid_config(serial_number, "fault-geometry-payload-out-of-range"));
        }
        break;
    case MockFaultKind::pixel_format_change:
        if (!fault.pixel_format || bytes_per_pixel(*fault.pixel_format) == 0U)
        {
            return Result<void>::failure(
                invalid_config(serial_number, "missing-or-invalid-fault-pixel-format"));
        }
        break;
    case MockFaultKind::disconnect:
    case MockFaultKind::timeout:
    case MockFaultKind::incomplete_frame:
        break;
    default:
        return Result<void>::failure(invalid_config(serial_number, "unknown-fault-kind"));
    }
    return Result<void>::success();
}

std::uint64_t mix(std::uint64_t value) noexcept
{
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

std::uint16_t sample_value(const MockFramePattern pattern, const std::uint64_t seed,
                           const std::uint64_t frame_number, const std::uint32_t x,
                           const std::uint32_t y, const std::uint16_t maximum) noexcept
{
    switch (pattern)
    {
    case MockFramePattern::gradient:
        return static_cast<std::uint16_t>((static_cast<std::uint64_t>(x) + y + frame_number) %
                                          (static_cast<std::uint64_t>(maximum) + 1U));
    case MockFramePattern::checkerboard:
        return (((x / 8U) + (y / 8U) + static_cast<std::uint32_t>(frame_number & 1U)) & 1U) == 0U
                   ? 0U
                   : maximum;
    case MockFramePattern::noise:
        return static_cast<std::uint16_t>(mix(seed ^ (frame_number * 0x9e3779b97f4a7c15ULL) ^
                                              (static_cast<std::uint64_t>(y) << 32U) ^ x) &
                                          maximum);
    }
    return 0U;
}

void fill_pattern(FrameBuffer& destination, const FrameGeometry geometry, const PixelFormat format,
                  const MockFramePattern pattern, const std::uint64_t seed,
                  const std::uint64_t frame_number, const bool reverse_x,
                  const bool reverse_y) noexcept
{
    auto bytes = destination.writable_bytes();
    std::fill_n(bytes.begin(), static_cast<std::size_t>(geometry.height) * geometry.stride,
                std::byte{0});
    const bool sixteen_bit = format == PixelFormat::mono10 || format == PixelFormat::mono12;
    const std::uint16_t maximum =
        format == PixelFormat::mono10 ? 1023U : (format == PixelFormat::mono12 ? 4095U : 255U);
    for (std::uint32_t y = 0U; y < geometry.height; ++y)
    {
        const auto row = static_cast<std::size_t>(y) * geometry.stride;
        for (std::uint32_t x = 0U; x < geometry.width; ++x)
        {
            const auto source_x = reverse_x ? geometry.width - 1U - x : x;
            const auto source_y = reverse_y ? geometry.height - 1U - y : y;
            const auto value =
                sample_value(pattern, seed, frame_number, source_x, source_y, maximum);
            if (sixteen_bit)
            {
                const auto offset = row + static_cast<std::size_t>(x) * 2U;
                bytes[offset] = static_cast<std::byte>(value & 0xffU);
                bytes[offset + 1U] = static_cast<std::byte>(value >> 8U);
            }
            else
            {
                bytes[row + x] = static_cast<std::byte>(value);
            }
        }
    }
}
} // namespace

namespace detail
{
struct MockSharedState final
{
    explicit MockSharedState(MockCameraConfig value)
        : config(std::move(value)), fault_slots(config.fault_queue_capacity)
    {
        const auto base_geometry = make_geometry(config.width, config.height, config.pixel_format);
        const auto base_size = payload_size(*base_geometry);
        if (config.maximum_payload_bytes == 0U)
        {
            config.maximum_payload_bytes = *base_size;
        }
        capabilities = {.exposure_us = SteppedRange<double>{1.0, 1000000.0, 1.0},
                        .gain_db = SteppedRange<double>{0.0, 48.0, 0.1},
                        .frame_rate = SteppedRange<double>{0.001, 1000.0, 0.001},
                        .roi = RoiCapabilities{.sensor_width = config.width,
                                               .sensor_height = config.height,
                                               .width = {1U, config.width, 1U},
                                               .height = {1U, config.height, 1U},
                                               .offset_x = {0U, config.width - 1U, 1U},
                                               .offset_y = {0U, config.height - 1U, 1U}},
                        .supports_reverse_x = true,
                        .supports_reverse_y = true,
                        .pixel_formats = {PixelFormat::mono8, PixelFormat::mono10,
                                          PixelFormat::mono12, PixelFormat::bayer_rg8},
                        .trigger_modes = {TriggerMode::continuous, TriggerMode::hardware,
                                          TriggerMode::software},
                        .trigger_sources = {"Line0"},
                        .trigger_delay_us = SteppedRange<std::uint32_t>{0U, 60000000U, 1U},
                        .packet_size_bytes = SteppedRange<std::uint32_t>{576U, 9000U, 1U},
                        .inter_packet_delay_ns = SteppedRange<std::uint32_t>{0U, 1000000U, 1U},
                        .maximum_payload_bytes = config.maximum_payload_bytes};
        parameters = {.exposure_us = 1000.0,
                      .gain_db = 0.0,
                      .frame_rate = config.frame_rate,
                      .roi = Roi{config.width, config.height, 0U, 0U},
                      .reverse_x = false,
                      .reverse_y = false,
                      .pixel_format = config.pixel_format,
                      .trigger_mode = config.trigger_mode,
                      .trigger_source = config.trigger_mode == TriggerMode::hardware
                                            ? std::optional<std::string>{"Line0"}
                                            : std::nullopt,
                      .trigger_delay_us = 0U,
                      .packet_size_bytes = 1500U,
                      .inter_packet_delay_ns = 0U};
        defaults = parameters;
    }

    MockCameraConfig config;
    CameraCapabilities capabilities;
    CameraParameterSnapshot parameters;
    CameraParameterSnapshot defaults;
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::vector<std::optional<MockFault>> fault_slots;
    std::size_t fault_head{};
    std::size_t fault_size{};
    std::size_t script_index{};
    std::size_t pending_software_triggers{};
    std::size_t pending_hardware_triggers{};
    bool connected{};
    bool streaming{};
    bool device_created{};
    std::uint64_t capture_attempts{};
    std::uint64_t frames_generated{};
    std::uint64_t camera_frame_number{};
    std::uint64_t faults_accepted{};
    std::uint64_t faults_rejected{};
    std::uint64_t faults_executed{};
    std::uint64_t triggers_rejected{};
    Clock::time_point next_frame_deadline{};
};
} // namespace detail

namespace
{
using SharedState = detail::MockSharedState;

Result<void> validate_configuration(MockCameraConfig& config)
{
    const auto& serial = config.descriptor.serial_number;
    if (serial.empty())
    {
        return Result<void>::failure(invalid_config(serial, "missing-serial-number"));
    }
    if (!std::isfinite(config.frame_rate) || config.frame_rate < 0.001 ||
        config.frame_rate > 1000.0 ||
        std::abs(config.frame_rate * 1000.0 - std::round(config.frame_rate * 1000.0)) > 1e-8)
    {
        return Result<void>::failure(invalid_config(serial, "frame-rate-out-of-range-or-step"));
    }
    switch (config.trigger_mode)
    {
    case TriggerMode::continuous:
    case TriggerMode::hardware:
    case TriggerMode::software:
        break;
    default:
        return Result<void>::failure(invalid_config(serial, "unknown-trigger-mode"));
    }
    switch (config.pattern)
    {
    case MockFramePattern::gradient:
    case MockFramePattern::checkerboard:
    case MockFramePattern::noise:
        break;
    default:
        return Result<void>::failure(invalid_config(serial, "unknown-frame-pattern"));
    }
    const auto geometry = make_geometry(config.width, config.height, config.pixel_format);
    if (!geometry)
    {
        return Result<void>::failure(invalid_config(serial, "invalid-frame-geometry"));
    }
    const auto base_payload = payload_size(*geometry);
    if (!base_payload)
    {
        return Result<void>::failure(invalid_config(serial, "frame-payload-overflow"));
    }
    if (config.maximum_payload_bytes == 0U)
    {
        config.maximum_payload_bytes = *base_payload;
    }
    if (config.maximum_payload_bytes < *base_payload ||
        config.maximum_payload_bytes > maximum_mock_payload_bytes)
    {
        return Result<void>::failure(invalid_config(serial, "maximum-payload-too-small"));
    }
    if (config.fault_queue_capacity == 0U ||
        config.fault_queue_capacity > maximum_mock_control_capacity)
    {
        return Result<void>::failure(invalid_config(serial, "fault-capacity-out-of-range"));
    }
    if (config.trigger_capacity == 0U || config.trigger_capacity > maximum_mock_control_capacity)
    {
        return Result<void>::failure(invalid_config(serial, "trigger-capacity-out-of-range"));
    }
    if (config.fault_script.size() > maximum_mock_control_capacity)
    {
        return Result<void>::failure(invalid_config(serial, "fault-script-too-large"));
    }
    std::uint64_t previous_attempt = 0U;
    for (const auto& scheduled : config.fault_script)
    {
        if (scheduled.capture_attempt == 0U || scheduled.capture_attempt <= previous_attempt)
        {
            return Result<void>::failure(
                invalid_config(serial, "fault-script-not-strictly-ordered"));
        }
        if (auto result = validate_fault(scheduled.fault, config.maximum_payload_bytes, serial);
            !result)
        {
            return result;
        }
        previous_attempt = scheduled.capture_attempt;
    }
    return Result<void>::success();
}

void reset_triggers(SharedState& state) noexcept
{
    state.pending_software_triggers = 0U;
    state.pending_hardware_triggers = 0U;
}

std::optional<MockFault> pop_runtime_fault(SharedState& state)
{
    if (state.fault_size == 0U)
    {
        return std::nullopt;
    }
    auto fault = std::move(state.fault_slots[state.fault_head]);
    state.fault_slots[state.fault_head].reset();
    state.fault_head = (state.fault_head + 1U) % state.fault_slots.size();
    --state.fault_size;
    return fault;
}

std::optional<MockFault> next_fault(SharedState& state)
{
    if (state.script_index < state.config.fault_script.size() &&
        state.config.fault_script[state.script_index].capture_attempt == state.capture_attempts)
    {
        return state.config.fault_script[state.script_index++].fault;
    }
    return pop_runtime_fault(state);
}

Result<void> require_connected(const SharedState& state, const std::string& operation)
{
    if (!state.connected)
    {
        return Result<void>::failure(
            invalid_state(state.config.descriptor.serial_number, operation, "not-connected"));
    }
    return Result<void>::success();
}

class MockCameraDevice final : public ICameraDevice
{
  public:
    explicit MockCameraDevice(std::shared_ptr<SharedState> state) : state_(std::move(state)) {}

    ~MockCameraDevice() override
    {
        std::lock_guard lock{state_->mutex};
        state_->streaming = false;
        state_->connected = false;
        state_->device_created = false;
        reset_triggers(*state_);
        state_->condition.notify_all();
    }

    [[nodiscard]] const CameraDeviceDescriptor& descriptor() const noexcept override
    {
        return state_->config.descriptor;
    }

    [[nodiscard]] Result<void> connect() override
    {
        std::lock_guard lock{state_->mutex};
        if (state_->connected)
        {
            return Result<void>::failure(invalid_state(state_->config.descriptor.serial_number,
                                                       "camera.mock.connect", "already-connected"));
        }
        state_->connected = true;
        return Result<void>::success();
    }

    [[nodiscard]] Result<void> disconnect() override
    {
        std::lock_guard lock{state_->mutex};
        if (!state_->connected)
        {
            return Result<void>::failure(invalid_state(state_->config.descriptor.serial_number,
                                                       "camera.mock.disconnect",
                                                       "already-disconnected"));
        }
        state_->streaming = false;
        state_->connected = false;
        reset_triggers(*state_);
        state_->condition.notify_all();
        return Result<void>::success();
    }

    [[nodiscard]] Result<CameraCapabilities> capabilities() override
    {
        std::lock_guard lock{state_->mutex};
        if (auto connected = require_connected(*state_, "camera.mock.capabilities"); !connected)
        {
            return Result<CameraCapabilities>::failure(connected.error());
        }
        return Result<CameraCapabilities>::success(state_->capabilities);
    }

    [[nodiscard]] Result<CameraParameterSnapshot> read_parameters() override
    {
        std::lock_guard lock{state_->mutex};
        if (auto connected = require_connected(*state_, "camera.mock.readParameters"); !connected)
        {
            return Result<CameraParameterSnapshot>::failure(connected.error());
        }
        return Result<CameraParameterSnapshot>::success(state_->parameters);
    }

    [[nodiscard]] Result<CameraParameterSnapshot> apply_parameters(
        const CameraParameterSnapshot& parameters) override
    {
        if (auto validation = validate_parameters(state_->capabilities, parameters); !validation)
        {
            return Result<CameraParameterSnapshot>::failure(validation.error());
        }

        std::lock_guard lock{state_->mutex};
        if (auto connected = require_connected(*state_, "camera.mock.applyParameters"); !connected)
        {
            return Result<CameraParameterSnapshot>::failure(connected.error());
        }
        if (state_->streaming)
        {
            return Result<CameraParameterSnapshot>::failure(
                invalid_state(state_->config.descriptor.serial_number,
                              "camera.mock.applyParameters", "streaming"));
        }

        auto candidate = state_->parameters;
        if (parameters.exposure_us)
            candidate.exposure_us = parameters.exposure_us;
        if (parameters.gain_db)
            candidate.gain_db = parameters.gain_db;
        if (parameters.frame_rate)
            candidate.frame_rate = parameters.frame_rate;
        if (parameters.roi)
            candidate.roi = parameters.roi;
        if (parameters.reverse_x)
            candidate.reverse_x = parameters.reverse_x;
        if (parameters.reverse_y)
            candidate.reverse_y = parameters.reverse_y;
        if (parameters.pixel_format)
            candidate.pixel_format = parameters.pixel_format;
        if (parameters.trigger_mode)
        {
            candidate.trigger_mode = parameters.trigger_mode;
            if (*parameters.trigger_mode != TriggerMode::hardware)
                candidate.trigger_source.reset();
        }
        if (parameters.trigger_source)
            candidate.trigger_source = parameters.trigger_source;
        if (parameters.trigger_delay_us)
            candidate.trigger_delay_us = parameters.trigger_delay_us;
        if (parameters.packet_size_bytes)
            candidate.packet_size_bytes = parameters.packet_size_bytes;
        if (parameters.inter_packet_delay_ns)
            candidate.inter_packet_delay_ns = parameters.inter_packet_delay_ns;
        if (!parameters.digital_io.empty())
            candidate.digital_io = parameters.digital_io;

        if (auto validation = validate_parameters(state_->capabilities, candidate); !validation)
        {
            return Result<CameraParameterSnapshot>::failure(validation.error());
        }
        const auto geometry =
            make_geometry(candidate.roi->width, candidate.roi->height, *candidate.pixel_format);
        const auto size = geometry ? payload_size(*geometry) : std::nullopt;
        if (!size || *size > state_->config.maximum_payload_bytes)
        {
            return Result<CameraParameterSnapshot>::failure(invalid_config(
                state_->config.descriptor.serial_number, "parameter-payload-exceeds-maximum"));
        }
        state_->parameters = std::move(candidate);
        reset_triggers(*state_);
        state_->condition.notify_all();
        return Result<CameraParameterSnapshot>::success(state_->parameters);
    }

    [[nodiscard]] Result<void> start_acquisition() override
    {
        std::lock_guard lock{state_->mutex};
        if (auto connected = require_connected(*state_, "camera.mock.start"); !connected)
        {
            return connected;
        }
        if (state_->streaming)
        {
            return Result<void>::failure(invalid_state(state_->config.descriptor.serial_number,
                                                       "camera.mock.start", "already-streaming"));
        }
        state_->streaming = true;
        reset_triggers(*state_);
        state_->next_frame_deadline = Clock::now();
        state_->condition.notify_all();
        return Result<void>::success();
    }

    [[nodiscard]] Result<CapturedFrameMetadata> capture_into(
        FrameBuffer& destination, const std::chrono::milliseconds timeout) override
    {
        std::unique_lock lock{state_->mutex};
        if (!state_->connected)
        {
            return disconnected();
        }
        if (!state_->streaming)
        {
            return Result<CapturedFrameMetadata>::failure(invalid_state(
                state_->config.descriptor.serial_number, "camera.mock.capture", "not-streaming"));
        }
        if (timeout <= std::chrono::milliseconds::zero())
        {
            return timed_out();
        }

        const auto timeout_deadline = Clock::now() + timeout;
        const auto mode = *state_->parameters.trigger_mode;
        bool ready = false;
        if (mode == TriggerMode::continuous)
        {
            const auto wake_deadline = std::min(timeout_deadline, state_->next_frame_deadline);
            state_->condition.wait_until(lock, wake_deadline,
                                         [&] { return !state_->connected || !state_->streaming; });
            ready = !state_->connected || !state_->streaming ||
                    Clock::now() >= state_->next_frame_deadline;
        }
        else
        {
            ready = state_->condition.wait_until(lock, timeout_deadline, [&] {
                const auto pending = mode == TriggerMode::software
                                         ? state_->pending_software_triggers
                                         : state_->pending_hardware_triggers;
                return !state_->connected || !state_->streaming || pending > 0U;
            });
        }
        if (!ready)
        {
            return timed_out();
        }
        if (!state_->connected)
        {
            return disconnected();
        }
        if (!state_->streaming)
        {
            return Result<CapturedFrameMetadata>::failure(invalid_state(
                state_->config.descriptor.serial_number, "camera.mock.capture", "stopped"));
        }
        if (mode == TriggerMode::software)
            --state_->pending_software_triggers;
        else if (mode == TriggerMode::hardware)
            --state_->pending_hardware_triggers;

        ++state_->capture_attempts;
        auto geometry = make_geometry(state_->parameters.roi->width, state_->parameters.roi->height,
                                      *state_->parameters.pixel_format)
                            .value();
        auto format = *state_->parameters.pixel_format;
        FrameFlags flags;
        if (auto fault = next_fault(*state_))
        {
            ++state_->faults_executed;
            switch (fault->kind)
            {
            case MockFaultKind::disconnect:
                state_->streaming = false;
                state_->connected = false;
                reset_triggers(*state_);
                state_->condition.notify_all();
                return disconnected();
            case MockFaultKind::timeout:
                return timed_out();
            case MockFaultKind::drop_frame:
            case MockFaultKind::frame_number_jump:
                if (state_->camera_frame_number >
                    std::numeric_limits<std::uint64_t>::max() - fault->amount)
                {
                    return Result<CapturedFrameMetadata>::failure(invalid_config(
                        state_->config.descriptor.serial_number, "frame-number-overflow"));
                }
                state_->camera_frame_number += fault->amount;
                break;
            case MockFaultKind::incomplete_frame:
                flags.incomplete = true;
                break;
            case MockFaultKind::geometry_change:
                geometry = *fault->geometry;
                break;
            case MockFaultKind::pixel_format_change:
                format = *fault->pixel_format;
                geometry = make_geometry(geometry.width, geometry.height, format).value();
                break;
            }
        }

        const auto size = payload_size(geometry);
        if (!valid_layout_for_format(geometry, format) || !size ||
            *size > state_->config.maximum_payload_bytes || *size > destination.capacity())
        {
            return Result<CapturedFrameMetadata>::failure(invalid_config(
                state_->config.descriptor.serial_number, "destination-buffer-too-small"));
        }
        if (state_->camera_frame_number == std::numeric_limits<std::uint64_t>::max())
        {
            return Result<CapturedFrameMetadata>::failure(
                invalid_config(state_->config.descriptor.serial_number, "frame-number-overflow"));
        }
        ++state_->camera_frame_number;
        fill_pattern(destination, geometry, format, state_->config.pattern,
                     state_->config.random_seed, state_->camera_frame_number,
                     state_->parameters.reverse_x.value_or(false),
                     state_->parameters.reverse_y.value_or(false));
        static_cast<void>(destination.set_size(*size));
        ++state_->frames_generated;

        if (mode == TriggerMode::continuous)
        {
            const auto period = std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>{1.0 / *state_->parameters.frame_rate});
            const auto candidate = state_->next_frame_deadline + period;
            state_->next_frame_deadline =
                candidate > Clock::now() ? candidate : Clock::now() + period;
        }
        return Result<CapturedFrameMetadata>::success(
            {.camera_frame_number = state_->camera_frame_number,
             .camera_timestamp = CameraTimestamp{state_->camera_frame_number, 1U,
                                                 CameraTimestampQuality::unsynchronized},
             .geometry = geometry,
             .pixel_format = format,
             .flags = flags});
    }

    [[nodiscard]] Result<void> software_trigger() override
    {
        std::lock_guard lock{state_->mutex};
        if (!state_->connected || !state_->streaming ||
            state_->parameters.trigger_mode != TriggerMode::software)
        {
            return Result<void>::failure(invalid_state(state_->config.descriptor.serial_number,
                                                       "camera.mock.softwareTrigger",
                                                       "software-trigger-not-active"));
        }
        if (state_->pending_software_triggers == state_->config.trigger_capacity)
        {
            ++state_->triggers_rejected;
            return Result<void>::failure(
                invalid_config(state_->config.descriptor.serial_number, "trigger-queue-full"));
        }
        ++state_->pending_software_triggers;
        state_->condition.notify_one();
        return Result<void>::success();
    }

    [[nodiscard]] Result<void> stop_acquisition() override
    {
        std::lock_guard lock{state_->mutex};
        if (!state_->streaming)
        {
            return Result<void>::failure(invalid_state(state_->config.descriptor.serial_number,
                                                       "camera.mock.stop", "not-streaming"));
        }
        state_->streaming = false;
        reset_triggers(*state_);
        state_->condition.notify_all();
        return Result<void>::success();
    }

    [[nodiscard]] Result<void> save_user_set(std::string_view) override
    {
        return Result<void>::failure(
            invalid_config(state_->config.descriptor.serial_number, "user-sets-unsupported"));
    }

    [[nodiscard]] Result<CameraParameterSnapshot> restore_defaults() override
    {
        std::lock_guard lock{state_->mutex};
        if (auto connected = require_connected(*state_, "camera.mock.restoreDefaults"); !connected)
        {
            return Result<CameraParameterSnapshot>::failure(connected.error());
        }
        if (state_->streaming)
        {
            return Result<CameraParameterSnapshot>::failure(
                invalid_state(state_->config.descriptor.serial_number,
                              "camera.mock.restoreDefaults", "streaming"));
        }
        state_->parameters = state_->defaults;
        reset_triggers(*state_);
        return Result<CameraParameterSnapshot>::success(state_->parameters);
    }

  private:
    Result<CapturedFrameMetadata> timed_out() const
    {
        return Result<CapturedFrameMetadata>::failure(
            make_camera_error(CameraErrorKind::frame_timeout, "模拟相机取帧超时",
                              "camera.mock.capture", state_->config.descriptor.serial_number));
    }

    Result<CapturedFrameMetadata> disconnected() const
    {
        return Result<CapturedFrameMetadata>::failure(
            make_camera_error(CameraErrorKind::disconnected, "模拟相机已断开",
                              "camera.mock.capture", state_->config.descriptor.serial_number));
    }

    std::shared_ptr<SharedState> state_;
};
} // namespace

MockCameraControl::MockCameraControl(std::weak_ptr<detail::MockSharedState> state)
    : state_(std::move(state))
{
}

Result<void> MockCameraControl::inject_fault(MockFault fault) const
{
    auto state = state_.lock();
    if (!state)
    {
        return Result<void>::failure(invalid_config({}, "control-expired"));
    }
    if (auto validation = validate_fault(fault, state->config.maximum_payload_bytes,
                                         state->config.descriptor.serial_number);
        !validation)
    {
        return validation;
    }
    std::lock_guard lock{state->mutex};
    if (state->fault_size == state->fault_slots.size())
    {
        ++state->faults_rejected;
        return Result<void>::failure(
            invalid_config(state->config.descriptor.serial_number, "fault-queue-full"));
    }
    const auto tail = (state->fault_head + state->fault_size) % state->fault_slots.size();
    state->fault_slots[tail] = std::move(fault);
    ++state->fault_size;
    ++state->faults_accepted;
    state->condition.notify_all();
    return Result<void>::success();
}

Result<void> MockCameraControl::hardware_trigger(const std::size_t count) const
{
    auto state = state_.lock();
    if (!state)
    {
        return Result<void>::failure(invalid_config({}, "control-expired"));
    }
    std::lock_guard lock{state->mutex};
    if (count == 0U || !state->connected || !state->streaming ||
        state->parameters.trigger_mode != TriggerMode::hardware)
    {
        return Result<void>::failure(invalid_state(state->config.descriptor.serial_number,
                                                   "camera.mock.hardwareTrigger",
                                                   "hardware-trigger-not-active"));
    }
    if (count > state->config.trigger_capacity - state->pending_hardware_triggers)
    {
        state->triggers_rejected += count;
        return Result<void>::failure(
            invalid_config(state->config.descriptor.serial_number, "trigger-queue-full"));
    }
    state->pending_hardware_triggers += count;
    state->condition.notify_all();
    return Result<void>::success();
}

void MockCameraControl::clear_faults() const noexcept
{
    if (auto state = state_.lock())
    {
        std::lock_guard lock{state->mutex};
        for (auto& fault : state->fault_slots)
        {
            fault.reset();
        }
        state->fault_head = 0U;
        state->fault_size = 0U;
    }
}

Result<MockCameraControlSnapshot> MockCameraControl::snapshot() const
{
    auto state = state_.lock();
    if (!state)
    {
        return Result<MockCameraControlSnapshot>::failure(invalid_config({}, "control-expired"));
    }
    std::lock_guard lock{state->mutex};
    return Result<MockCameraControlSnapshot>::success(
        {.connected = state->connected,
         .streaming = state->streaming,
         .capture_attempts = state->capture_attempts,
         .frames_generated = state->frames_generated,
         .camera_frame_number = state->camera_frame_number,
         .queued_faults = state->fault_size,
         .faults_accepted = state->faults_accepted,
         .faults_rejected = state->faults_rejected,
         .faults_executed = state->faults_executed,
         .pending_software_triggers = state->pending_software_triggers,
         .pending_hardware_triggers = state->pending_hardware_triggers,
         .triggers_rejected = state->triggers_rejected});
}

MockCameraProvider::MockCameraProvider(ValidatedTag,
                                       std::vector<std::shared_ptr<detail::MockSharedState>> states)
    : states_(std::move(states))
{
}

MockCameraProvider::~MockCameraProvider() = default;

Result<std::unique_ptr<MockCameraProvider>> MockCameraProvider::create(
    std::vector<MockCameraConfig> configurations)
{
    if (configurations.empty() || configurations.size() > maximum_mock_camera_count)
    {
        return Result<std::unique_ptr<MockCameraProvider>>::failure(
            invalid_config({}, "camera-count-out-of-range"));
    }
    std::set<std::string> serials;
    std::vector<std::shared_ptr<detail::MockSharedState>> states;
    states.reserve(configurations.size());
    for (auto& configuration : configurations)
    {
        if (auto validation = validate_configuration(configuration); !validation)
        {
            return Result<std::unique_ptr<MockCameraProvider>>::failure(validation.error());
        }
        if (!serials.insert(configuration.descriptor.serial_number).second)
        {
            return Result<std::unique_ptr<MockCameraProvider>>::failure(
                invalid_config(configuration.descriptor.serial_number, "duplicate-serial-number"));
        }
        states.push_back(std::make_shared<detail::MockSharedState>(std::move(configuration)));
    }
    return Result<std::unique_ptr<MockCameraProvider>>::success(
        std::make_unique<MockCameraProvider>(ValidatedTag{}, std::move(states)));
}

Result<std::vector<CameraDeviceDescriptor>> MockCameraProvider::enumerate_devices()
{
    std::vector<CameraDeviceDescriptor> devices;
    devices.reserve(states_.size());
    for (const auto& state : states_)
    {
        devices.push_back(state->config.descriptor);
    }
    return Result<std::vector<CameraDeviceDescriptor>>::success(std::move(devices));
}

Result<std::unique_ptr<ICameraDevice>> MockCameraProvider::create_device(
    const std::string_view serial_number)
{
    const auto match = std::find_if(states_.begin(), states_.end(), [&](const auto& state) {
        return state->config.descriptor.serial_number == serial_number;
    });
    if (match == states_.end())
    {
        return Result<std::unique_ptr<ICameraDevice>>::failure(make_camera_error(
            CameraErrorKind::not_found, "未发现模拟相机", "camera.mock.createDevice"));
    }
    {
        std::lock_guard lock{(*match)->mutex};
        if ((*match)->device_created)
        {
            return Result<std::unique_ptr<ICameraDevice>>::failure(
                invalid_state((*match)->config.descriptor.serial_number, "camera.mock.createDevice",
                              "device-already-created"));
        }
        (*match)->device_created = true;
    }
    try
    {
        std::unique_ptr<ICameraDevice> device = std::make_unique<MockCameraDevice>(*match);
        return Result<std::unique_ptr<ICameraDevice>>::success(std::move(device));
    }
    catch (...)
    {
        std::lock_guard lock{(*match)->mutex};
        (*match)->device_created = false;
        throw;
    }
}

Result<MockCameraControl> MockCameraProvider::control(const std::string_view serial_number) const
{
    const auto match = std::find_if(states_.begin(), states_.end(), [&](const auto& state) {
        return state->config.descriptor.serial_number == serial_number;
    });
    if (match == states_.end())
    {
        return Result<MockCameraControl>::failure(
            make_camera_error(CameraErrorKind::not_found, "未发现模拟相机", "camera.mock.control"));
    }
    return Result<MockCameraControl>::success(MockCameraControl{*match});
}

} // namespace paperbreak::camera::mock
