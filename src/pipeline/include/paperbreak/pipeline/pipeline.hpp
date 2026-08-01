#pragma once

#include "paperbreak/camera/acquisition.hpp"
#include "paperbreak/camera/frame.hpp"
#include "paperbreak/common/result.hpp"

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

namespace paperbreak::pipeline
{

struct GrayStatistics final
{
    double minimum{};
    double maximum{};
    double mean{};
    std::uint64_t sample_count{};
    bool operator==(const GrayStatistics&) const = default;
};

struct ProcessedFrame final
{
    camera::FrameView frame;
    bool validity_checked{};
    std::optional<GrayStatistics> gray_statistics;
};

class IPreprocessingNode
{
  public:
    virtual ~IPreprocessingNode() = default;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual Result<void> process(ProcessedFrame& frame) = 0;
};

class PassThroughNode final : public IPreprocessingNode
{
  public:
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] Result<void> process(ProcessedFrame& frame) override;
};

struct ValidityCheckOptions final
{
    std::optional<camera::FrameGeometry> expected_geometry;
    std::optional<camera::PixelFormat> expected_pixel_format;
    bool reject_incomplete{true};
};

class ValidityCheckNode final : public IPreprocessingNode
{
  public:
    explicit ValidityCheckNode(ValidityCheckOptions options = {});
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] Result<void> process(ProcessedFrame& frame) override;

  private:
    ValidityCheckOptions options_;
};

class GrayStatisticsNode final : public IPreprocessingNode
{
  public:
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] Result<void> process(ProcessedFrame& frame) override;
};

class PreprocessingChain final
{
  public:
    explicit PreprocessingChain(std::vector<std::unique_ptr<IPreprocessingNode>> nodes);

    PreprocessingChain(const PreprocessingChain&) = delete;
    PreprocessingChain& operator=(const PreprocessingChain&) = delete;
    PreprocessingChain(PreprocessingChain&&) noexcept = default;
    PreprocessingChain& operator=(PreprocessingChain&&) noexcept = default;

    [[nodiscard]] Result<void> process(ProcessedFrame& frame);
    [[nodiscard]] std::size_t size() const noexcept;

  private:
    std::vector<std::unique_ptr<IPreprocessingNode>> nodes_;
};

enum class AlgorithmEnqueueStatus
{
    enqueued,
    enqueued_after_skipping_oldest,
    closed,
};

enum class AlgorithmDequeueStatus
{
    frame,
    timeout,
    stopped,
    closed,
};

struct AlgorithmDequeueResult final
{
    AlgorithmDequeueStatus status{AlgorithmDequeueStatus::closed};
    std::optional<ProcessedFrame> frame;
};

struct AlgorithmQueueSnapshot final
{
    std::size_t capacity{};
    std::size_t depth{};
    std::size_t high_watermark{};
    std::uint64_t enqueued{};
    std::uint64_t dequeued{};
    std::uint64_t algorithm_skipped{};
    std::uint64_t rejected_closed{};
    std::uint64_t wait_timeouts{};
    std::uint64_t wait_cancelled{};
    bool closed{};
};

class AlgorithmQueue final
{
  public:
    explicit AlgorithmQueue(std::size_t capacity = 8U);

    AlgorithmQueue(const AlgorithmQueue&) = delete;
    AlgorithmQueue& operator=(const AlgorithmQueue&) = delete;

    [[nodiscard]] AlgorithmEnqueueStatus push(ProcessedFrame frame) noexcept;
    [[nodiscard]] AlgorithmDequeueResult wait_pop(std::stop_token stop_token,
                                                  std::chrono::milliseconds timeout) noexcept;
    void close() noexcept;
    [[nodiscard]] AlgorithmQueueSnapshot snapshot() const noexcept;

  private:
    mutable std::mutex mutex_;
    std::condition_variable_any condition_;
    std::vector<std::optional<ProcessedFrame>> slots_;
    std::size_t head_{};
    std::size_t size_{};
    std::atomic<std::size_t> depth_{};
    std::atomic<std::size_t> high_watermark_{};
    std::atomic<std::uint64_t> enqueued_{};
    std::atomic<std::uint64_t> dequeued_{};
    std::atomic<std::uint64_t> algorithm_skipped_{};
    std::atomic<std::uint64_t> rejected_closed_{};
    std::atomic<std::uint64_t> wait_timeouts_{};
    std::atomic<std::uint64_t> wait_cancelled_{};
    std::atomic<bool> closed_{};
};

struct PerCameraProcessorOptions final
{
    std::string camera_id;
    std::chrono::milliseconds input_wait_timeout{std::chrono::milliseconds{100}};
};

struct PerCameraProcessorSnapshot final
{
    bool started{};
    bool running{};
    bool completed{true};
    std::uint64_t frames_processed{};
    std::uint64_t frames_rejected{};
    std::uint64_t invalid_frames{};
    std::uint64_t sequence_gaps{};
    std::uint64_t node_failures{};
    double average_processing_microseconds{};
    double maximum_processing_microseconds{};
    AlgorithmQueueSnapshot algorithm_queue;
    std::optional<Error> last_error;
};

class PerCameraProcessor final
{
  public:
    PerCameraProcessor(camera::AcquisitionQueue& input, AlgorithmQueue& output,
                       PreprocessingChain chain, PerCameraProcessorOptions options);
    ~PerCameraProcessor();

    PerCameraProcessor(const PerCameraProcessor&) = delete;
    PerCameraProcessor& operator=(const PerCameraProcessor&) = delete;

    [[nodiscard]] Result<void> start();
    void request_stop() noexcept;
    [[nodiscard]] Result<void> join(std::chrono::steady_clock::time_point deadline);
    [[nodiscard]] PerCameraProcessorSnapshot snapshot() const;
    [[nodiscard]] const std::string& camera_id() const noexcept;

  private:
    void run(std::stop_token stop_token) noexcept;
    void finish(std::optional<Error> error) noexcept;
    void record_failure(const Error& error) noexcept;

    camera::AcquisitionQueue& input_;
    AlgorithmQueue& output_;
    PreprocessingChain chain_;
    PerCameraProcessorOptions options_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool started_{};
    bool running_{};
    bool completed_{true};
    std::optional<Error> last_error_;
    std::atomic<std::uint64_t> frames_processed_{};
    std::atomic<std::uint64_t> frames_rejected_{};
    std::atomic<std::uint64_t> invalid_frames_{};
    std::atomic<std::uint64_t> sequence_gaps_{};
    std::atomic<std::uint64_t> node_failures_{};
    std::atomic<std::uint64_t> total_processing_nanoseconds_{};
    std::atomic<std::uint64_t> maximum_processing_nanoseconds_{};
    std::jthread worker_;
};

class ProcessingRuntime final
{
  public:
    [[nodiscard]] Result<void> add(std::unique_ptr<PerCameraProcessor> processor);
    [[nodiscard]] Result<void> start();
    void request_stop() noexcept;
    [[nodiscard]] Result<void> join(std::chrono::steady_clock::time_point deadline);
    [[nodiscard]] std::size_t size() const noexcept;

  private:
    std::vector<std::unique_ptr<PerCameraProcessor>> processors_;
    bool started_{};
};

} // namespace paperbreak::pipeline
