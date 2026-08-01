#include "paperbreak/camera/frame_pool.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace paperbreak::camera
{

struct FrameBufferPool::State final
{
    State(const std::size_t capacity, const std::size_t buffer_capacity_bytes)
        : capacity(capacity), buffer_capacity_bytes(buffer_capacity_bytes)
    {
        available.reserve(capacity);
        for (std::size_t index = 0U; index < capacity; ++index)
        {
            available.push_back(std::make_unique<FrameBuffer>(buffer_capacity_bytes));
        }
    }

    void release(FrameBuffer* buffer) noexcept
    {
        buffer->clear();
        {
            std::lock_guard lock{mutex};
            available.emplace_back(buffer);
        }
        condition.notify_one();
    }

    const std::size_t capacity;
    const std::size_t buffer_capacity_bytes;
    mutable std::mutex mutex;
    std::condition_variable_any condition;
    std::vector<std::unique_ptr<FrameBuffer>> available;
    std::size_t in_use_high_watermark{};
    std::uint64_t acquired{};
    std::uint64_t exhausted{};
    std::uint64_t timed_out{};
    std::uint64_t cancelled{};
    bool closed{};
};

FrameBufferPool::FrameBufferPool(const std::size_t capacity,
                                 const std::size_t buffer_capacity_bytes)
{
    if (capacity == 0U || buffer_capacity_bytes == 0U)
    {
        throw std::invalid_argument{"FrameBufferPool capacity must be non-zero"};
    }
    state_ = std::make_shared<State>(capacity, buffer_capacity_bytes);
}

FrameBufferPool::~FrameBufferPool()
{
    close();
}

FramePoolAcquireResult FrameBufferPool::acquire(const std::stop_token stop_token,
                                                const std::chrono::milliseconds timeout)
{
    auto state = state_;
    std::unique_lock lock{state->mutex};

    const auto take_available = [&]() -> FramePoolAcquireResult {
        auto owned = std::move(state->available.back());
        state->available.pop_back();
        auto* buffer = owned.release();
        ++state->acquired;
        const auto in_use = state->capacity - state->available.size();
        state->in_use_high_watermark = std::max(state->in_use_high_watermark, in_use);
        return {FramePoolAcquireStatus::acquired,
                std::shared_ptr<FrameBuffer>{
                    buffer, [state](FrameBuffer* released) noexcept { state->release(released); }}};
    };

    if (state->closed)
    {
        return {FramePoolAcquireStatus::closed, {}};
    }
    if (stop_token.stop_requested())
    {
        ++state->cancelled;
        return {FramePoolAcquireStatus::stopped, {}};
    }
    if (!state->available.empty())
    {
        return take_available();
    }
    if (timeout <= std::chrono::milliseconds::zero())
    {
        ++state->exhausted;
        return {FramePoolAcquireStatus::exhausted, {}};
    }

    const bool ready = state->condition.wait_for(
        lock, stop_token, timeout, [&] { return state->closed || !state->available.empty(); });
    if (!ready)
    {
        if (stop_token.stop_requested())
        {
            ++state->cancelled;
            return {FramePoolAcquireStatus::stopped, {}};
        }
        ++state->timed_out;
        return {FramePoolAcquireStatus::timeout, {}};
    }
    if (stop_token.stop_requested())
    {
        ++state->cancelled;
        return {FramePoolAcquireStatus::stopped, {}};
    }
    if (state->closed)
    {
        return {FramePoolAcquireStatus::closed, {}};
    }
    if (!state->available.empty())
    {
        return take_available();
    }
    ++state->timed_out;
    return {FramePoolAcquireStatus::timeout, {}};
}

void FrameBufferPool::close() noexcept
{
    auto state = state_;
    {
        std::lock_guard lock{state->mutex};
        state->closed = true;
    }
    state->condition.notify_all();
}

FrameBufferPoolSnapshot FrameBufferPool::snapshot() const noexcept
{
    const auto state = state_;
    std::lock_guard lock{state->mutex};
    return {.capacity = state->capacity,
            .buffer_capacity_bytes = state->buffer_capacity_bytes,
            .available = state->available.size(),
            .in_use = state->capacity - state->available.size(),
            .in_use_high_watermark = state->in_use_high_watermark,
            .acquired = state->acquired,
            .exhausted = state->exhausted,
            .timed_out = state->timed_out,
            .cancelled = state->cancelled,
            .closed = state->closed};
}

} // namespace paperbreak::camera
