#pragma once

#include "paperbreak/camera/camera.hpp"
#include "paperbreak/camera/frame_pool.hpp"
#include "paperbreak/camera/state.hpp"
#include "paperbreak/common/result.hpp"
#include "paperbreak/common/threading.hpp"

#include <atomic>
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
    std::atomic<std::size_t> depth_{};
    std::atomic<std::size_t> high_watermark_{};
    std::atomic<std::uint64_t> enqueued_{};
    std::atomic<std::uint64_t> dequeued_{};
    std::atomic<std::uint64_t> dropped_oldest_{};
    std::atomic<std::uint64_t> rejected_closed_{};
    std::atomic<std::uint64_t> wait_timeouts_{};
    std::atomic<std::uint64_t> wait_cancelled_{};
    std::atomic<bool> closed_{};
};

struct AcquisitionWorkerOptions final
{
    std::string camera_id;
    std::chrono::milliseconds receive_timeout{std::chrono::seconds{1}};
    std::chrono::milliseconds statistics_window{std::chrono::seconds{1}};
    std::size_t consecutive_timeout_limit{3U};
    std::optional<std::chrono::milliseconds> software_trigger_interval;
    ThreadRegistrationFactory register_thread;
    DebugDiagnosticSink diagnostics;
};

struct AcquisitionWorkerSnapshot final
{
    bool started{};
    bool running{};
    bool completed{true};
    std::uint64_t last_sequence_number{};
    std::uint64_t frames_received{};
    std::uint64_t camera_frame_gaps{};
    std::uint64_t capture_timeouts{};
    std::uint64_t incomplete_frames{};
    std::uint64_t bytes_received{};
    double actual_fps{};
    double bandwidth_bytes_per_second{};
    std::optional<MonotonicTime> last_frame_monotonic_time;
    std::optional<WallClockTime> last_frame_wall_clock_time;
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
    [[nodiscard]] bool wait_until_completed(
        std::chrono::steady_clock::time_point deadline) noexcept;
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
    std::condition_variable_any trigger_wait_condition_;
    bool started_{};
    bool running_{};
    bool completed_{true};
    std::uint64_t last_sequence_number_{};
    std::optional<Error> last_error_;
    std::atomic<std::uint64_t> frames_received_{};
    std::atomic<std::uint64_t> camera_frame_gaps_{};
    std::atomic<std::uint64_t> capture_timeouts_{};
    std::atomic<std::uint64_t> incomplete_frames_{};
    std::atomic<std::uint64_t> bytes_received_{};
    std::atomic<double> actual_fps_{};
    std::atomic<double> bandwidth_bytes_per_second_{};
    std::atomic<std::int64_t> last_frame_monotonic_ticks_{};
    std::atomic<std::int64_t> last_frame_wall_clock_ticks_{};
    std::atomic<bool> has_last_frame_{};
    std::jthread worker_;
};

struct RecoveringCameraSessionOptions final
{
    std::string camera_id;
    std::string serial_number;
    std::chrono::milliseconds receive_timeout{std::chrono::seconds{1}};
    std::chrono::milliseconds statistics_window{std::chrono::seconds{1}};
    std::size_t consecutive_timeout_limit{3U};
    ReconnectPolicy reconnect_policy;
    ThreadRegistrationFactory register_thread;
};

struct RecoveringCameraSessionSnapshot final
{
    bool started{};
    bool running{};
    bool completed{true};
    CameraStateSnapshot state;
    AcquisitionWorkerSnapshot acquisition;
    std::uint64_t connection_attempts{};
};

/// Owns one camera's connect/start/capture/cleanup/retry loop while using fixed external resources.
class RecoveringCameraSession final
{
  public:
    RecoveringCameraSession(ICameraProvider& provider, FrameBufferPool& pool,
                            AcquisitionQueue& queue, RecoveringCameraSessionOptions options,
                            CameraTransitionObserver observer = {}, ReconnectWaiter waiter = {});
    ~RecoveringCameraSession();

    RecoveringCameraSession(const RecoveringCameraSession&) = delete;
    RecoveringCameraSession& operator=(const RecoveringCameraSession&) = delete;

    [[nodiscard]] Result<void> start();
    void request_stop() noexcept;
    [[nodiscard]] Result<void> join(std::chrono::steady_clock::time_point deadline);
    [[nodiscard]] RecoveringCameraSessionSnapshot snapshot() const;

  private:
    void run(std::stop_token stop_token) noexcept;
    [[nodiscard]] bool recover_from(Error error, std::string reason,
                                    std::stop_token stop_token) noexcept;
    void finish() noexcept;

    ICameraProvider& provider_;
    FrameBufferPool& pool_;
    AcquisitionQueue& queue_;
    RecoveringCameraSessionOptions options_;
    CameraSessionController controller_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool started_{};
    bool running_{};
    bool completed_{true};
    AcquisitionWorker* active_worker_{};
    AcquisitionWorkerSnapshot last_acquisition_;
    std::atomic<std::uint64_t> connection_attempts_{};
    std::jthread worker_;
};

} // namespace paperbreak::camera
