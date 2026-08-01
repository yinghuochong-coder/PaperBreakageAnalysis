#pragma once

#include "paperbreak/common/result.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace paperbreak::camera
{

using MonotonicTime = std::chrono::steady_clock::time_point;
using WallClockTime = std::chrono::system_clock::time_point;

enum class PixelFormat
{
    mono8,
    mono10,
    mono12,
    bayer_rg8,
};

enum class CameraTimestampQuality
{
    unknown,
    unsynchronized,
    synchronized,
};

struct CameraTimestamp final
{
    std::uint64_t ticks{};
    std::uint64_t frequency_hz{};
    CameraTimestampQuality quality{CameraTimestampQuality::unknown};
    bool operator==(const CameraTimestamp&) const = default;
};

struct FrameGeometry final
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t stride{};
    bool operator==(const FrameGeometry&) const = default;
};

struct FrameFlags final
{
    bool incomplete{};
    bool operator==(const FrameFlags&) const = default;
};

/// Fixed-capacity image storage. Construction is the only operation that allocates.
class FrameBuffer final
{
  public:
    explicit FrameBuffer(std::size_t capacity);

    FrameBuffer(const FrameBuffer&) = delete;
    FrameBuffer& operator=(const FrameBuffer&) = delete;
    FrameBuffer(FrameBuffer&&) noexcept = default;
    FrameBuffer& operator=(FrameBuffer&&) noexcept = default;

    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::span<std::byte> writable_bytes() noexcept;
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept;

    /// Changes the logical payload length without reallocating.
    [[nodiscard]] bool set_size(std::size_t size) noexcept;
    void clear() noexcept;

  private:
    std::vector<std::byte> storage_;
    std::size_t size_{};
};

struct FramePacket final
{
    std::string camera_id;
    std::uint64_t camera_frame_number{};
    std::uint64_t sequence_number{};
    MonotonicTime received_monotonic_time;
    WallClockTime received_wall_clock_time;
    std::optional<CameraTimestamp> camera_timestamp;
    FrameGeometry geometry;
    PixelFormat pixel_format{PixelFormat::mono8};
    std::shared_ptr<const FrameBuffer> buffer;
    FrameFlags flags;
};

/// Immutable, owning projection of a frame suitable for downstream consumers.
class FrameView final
{
  public:
    [[nodiscard]] const std::string& camera_id() const noexcept;
    [[nodiscard]] std::uint64_t camera_frame_number() const noexcept;
    [[nodiscard]] std::uint64_t sequence_number() const noexcept;
    [[nodiscard]] MonotonicTime received_monotonic_time() const noexcept;
    [[nodiscard]] WallClockTime received_wall_clock_time() const noexcept;
    [[nodiscard]] const std::optional<CameraTimestamp>& camera_timestamp() const noexcept;
    [[nodiscard]] FrameGeometry geometry() const noexcept;
    [[nodiscard]] PixelFormat pixel_format() const noexcept;
    [[nodiscard]] FrameFlags flags() const noexcept;
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept;
    [[nodiscard]] const std::shared_ptr<const FrameBuffer>& buffer_owner() const noexcept;

  private:
    friend Result<FrameView> make_frame_view(const FramePacket& packet);

    std::string camera_id_;
    std::uint64_t camera_frame_number_{};
    std::uint64_t sequence_number_{};
    MonotonicTime received_monotonic_time_;
    WallClockTime received_wall_clock_time_;
    std::optional<CameraTimestamp> camera_timestamp_;
    FrameGeometry geometry_;
    PixelFormat pixel_format_{PixelFormat::mono8};
    std::shared_ptr<const FrameBuffer> buffer_;
    FrameFlags flags_;
};

/// Validates packet layout and creates a zero-copy read-only view.
[[nodiscard]] Result<FrameView> make_frame_view(const FramePacket& packet);

} // namespace paperbreak::camera
