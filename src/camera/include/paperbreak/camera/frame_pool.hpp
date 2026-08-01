#pragma once

#include "paperbreak/camera/frame.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <stop_token>

namespace paperbreak::camera
{

enum class FramePoolAcquireStatus
{
    acquired,
    exhausted,
    timeout,
    stopped,
    closed,
};

struct FramePoolAcquireResult final
{
    FramePoolAcquireStatus status{FramePoolAcquireStatus::closed};
    std::shared_ptr<FrameBuffer> buffer;
};

struct FrameBufferPoolSnapshot final
{
    std::size_t capacity{};
    std::size_t buffer_capacity_bytes{};
    std::size_t available{};
    std::size_t in_use{};
    std::size_t in_use_high_watermark{};
    std::uint64_t acquired{};
    std::uint64_t exhausted{};
    std::uint64_t timed_out{};
    std::uint64_t cancelled{};
    bool closed{};
};

/// Per-camera, fixed-capacity pool. Construction performs all FrameBuffer allocations.
class FrameBufferPool final
{
  public:
    FrameBufferPool(std::size_t capacity, std::size_t buffer_capacity_bytes);
    ~FrameBufferPool();

    FrameBufferPool(const FrameBufferPool&) = delete;
    FrameBufferPool& operator=(const FrameBufferPool&) = delete;
    FrameBufferPool(FrameBufferPool&&) = delete;
    FrameBufferPool& operator=(FrameBufferPool&&) = delete;

    [[nodiscard]] FramePoolAcquireResult acquire(std::stop_token stop_token,
                                                 std::chrono::milliseconds timeout);
    void close() noexcept;
    [[nodiscard]] FrameBufferPoolSnapshot snapshot() const noexcept;

  private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace paperbreak::camera
