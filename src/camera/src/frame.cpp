#include "paperbreak/camera/frame.hpp"

#include <limits>
#include <utility>

namespace paperbreak::camera
{
namespace
{
Error invalid_frame(std::string reason)
{
    auto error = make_error("CAMERA_CONFIG_FAILED", Severity::error, "帧布局无效", "camera",
                            "camera.createFrameView", false);
    error.details.push_back({"reason", std::move(reason)});
    return error;
}
} // namespace

FrameBuffer::FrameBuffer(const std::size_t capacity) : storage_(capacity) {}

std::size_t FrameBuffer::capacity() const noexcept
{
    return storage_.size();
}

std::size_t FrameBuffer::size() const noexcept
{
    return size_;
}

std::span<std::byte> FrameBuffer::writable_bytes() noexcept
{
    return storage_;
}

std::span<const std::byte> FrameBuffer::bytes() const noexcept
{
    return {storage_.data(), size_};
}

bool FrameBuffer::set_size(const std::size_t size) noexcept
{
    if (size > capacity())
    {
        return false;
    }
    size_ = size;
    return true;
}

void FrameBuffer::clear() noexcept
{
    size_ = 0U;
}

const std::string& FrameView::camera_id() const noexcept
{
    return camera_id_;
}

std::uint64_t FrameView::camera_frame_number() const noexcept
{
    return camera_frame_number_;
}

std::uint64_t FrameView::sequence_number() const noexcept
{
    return sequence_number_;
}

MonotonicTime FrameView::received_monotonic_time() const noexcept
{
    return received_monotonic_time_;
}

WallClockTime FrameView::received_wall_clock_time() const noexcept
{
    return received_wall_clock_time_;
}

const std::optional<CameraTimestamp>& FrameView::camera_timestamp() const noexcept
{
    return camera_timestamp_;
}

FrameGeometry FrameView::geometry() const noexcept
{
    return geometry_;
}

PixelFormat FrameView::pixel_format() const noexcept
{
    return pixel_format_;
}

FrameFlags FrameView::flags() const noexcept
{
    return flags_;
}

std::span<const std::byte> FrameView::bytes() const noexcept
{
    return buffer_->bytes();
}

const std::shared_ptr<const FrameBuffer>& FrameView::buffer_owner() const noexcept
{
    return buffer_;
}

Result<FrameView> make_frame_view(const FramePacket& packet)
{
    if (packet.camera_id.empty())
    {
        return Result<FrameView>::failure(invalid_frame("missing-camera-id"));
    }
    if (!packet.buffer)
    {
        return Result<FrameView>::failure(invalid_frame("missing-buffer"));
    }
    if (packet.geometry.width == 0U || packet.geometry.height == 0U || packet.geometry.stride == 0U)
    {
        return Result<FrameView>::failure(invalid_frame("invalid-geometry"));
    }
    if (packet.geometry.height > std::numeric_limits<std::size_t>::max() / packet.geometry.stride)
    {
        return Result<FrameView>::failure(invalid_frame("payload-size-overflow"));
    }

    const auto required_size = static_cast<std::size_t>(packet.geometry.height) *
                               static_cast<std::size_t>(packet.geometry.stride);
    if (packet.buffer->size() != required_size)
    {
        return Result<FrameView>::failure(invalid_frame("payload-size-mismatch"));
    }

    FrameView view;
    view.camera_id_ = packet.camera_id;
    view.camera_frame_number_ = packet.camera_frame_number;
    view.sequence_number_ = packet.sequence_number;
    view.received_monotonic_time_ = packet.received_monotonic_time;
    view.received_wall_clock_time_ = packet.received_wall_clock_time;
    view.camera_timestamp_ = packet.camera_timestamp;
    view.geometry_ = packet.geometry;
    view.pixel_format_ = packet.pixel_format;
    view.buffer_ = packet.buffer;
    view.flags_ = packet.flags;
    return Result<FrameView>::success(std::move(view));
}

} // namespace paperbreak::camera
