#pragma once

#include "paperbreak/camera/camera.hpp"
#include "paperbreak/camera/frame_pool.hpp"
#include "paperbreak/common/result.hpp"

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
#include <vector>

namespace paperbreak::camera
{

enum class FrameEnqueueStatus
{
    enqueued,
    enqueued_after_dropping_oldest,
    closed,
};

enum class FrameDequeueStatus
{
    frame,
    timeout,
    stopped,
    closed,
};

struct FrameDequeueResult final
{
    FrameDequeueStatus status{FrameDequeueStatus::closed};
    std::optional<FramePacket> packet;
};

struct AcquisitionQueueSnapshot final
{
    std::size_t capacity{};
    std::size_t depth{};
    std::size_t high_watermark{};
    std::uint64_t enqueued{};
    std::uint64_t dequeued{};
    std::uint64_t dropped_oldest{};
    std::uint64_t rejected_closed{};
    std::uint64_t wait_timeouts{};
    std::uint64_t wait_cancelled{};
    bool closed{};
};

/// Single-camera bounded SPSC-compatible queue with drop-oldest overflow semantics.
class AcquisitionQueue final
{
  public:
    explicit AcquisitionQueue(std::size_t capacity);

    AcquisitionQueue(const AcquisitionQueue&) = delete;
    AcquisitionQueue& operator=(const AcquisitionQueue&) = delete;

    [[nodiscard]] FrameEnqueueStatus push(FramePacket packet) noexcept;
    [[nodiscard]] FrameDequeueResult wait_pop(std::stop_token stop_token,
                                              std::chrono::milliseconds timeout) noexcept;
    void close() noexcept;
    [[nodiscard]] AcquisitionQueueSnapshot snapshot() const noexcept;

  private:
    mutable std::mutex mutex_;
    std::condition_variable_any condition_;
    std::vector<std::optional<FramePacket>> slots_;
    std::size_t head_{};
    std::size_t size_{};
    std::size_t high_watermark_{};
    std::uint64_t enqueued_{};
    std::uint64_t dequeued_{};
    std::uint64_t dropped_oldest_{};
    std::uint64_t rejected_closed_{};
    std::uint64_t wait_timeouts_{};
    std::uint64_t wait_cancelled_{};
    bool closed_{};
};

struct AcquisitionWorkerOptions final
{
    std::string camera_id;
    std::chrono::milliseconds receive_timeout{std::chrono::seconds{1}};
};

struct AcquisitionWorkerSnapshot final
{
    bool started{};
    bool running{};
    bool completed{true};
    std::uint64_t last_sequence_number{};
    std::optional<Error> last_error;
};

/// Pulls frames from an already-streaming device and publishes them to one bounded queue.
class AcquisitionWorker final
{
  public:
    AcquisitionWorker(ICameraDevice& device, FrameBufferPool& pool, AcquisitionQueue& queue,
                      AcquisitionWorkerOptions options);
    ~AcquisitionWorker();

    AcquisitionWorker(const AcquisitionWorker&) = delete;
    AcquisitionWorker& operator=(const AcquisitionWorker&) = delete;

    [[nodiscard]] Result<void> start();
    void request_stop() noexcept;
    [[nodiscard]] Result<void> join(std::chrono::steady_clock::time_point deadline);
    [[nodiscard]] AcquisitionWorkerSnapshot snapshot() const;

  private:
    void run(std::stop_token stop_token) noexcept;
    void finish(std::optional<Error> error, std::uint64_t last_sequence_number) noexcept;

    ICameraDevice& device_;
    FrameBufferPool& pool_;
    AcquisitionQueue& queue_;
    AcquisitionWorkerOptions options_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool started_{};
    bool running_{};
    bool completed_{true};
    std::uint64_t last_sequence_number_{};
    std::optional<Error> last_error_;
    std::jthread worker_;
};

} // namespace paperbreak::camera
