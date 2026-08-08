#pragma once

#include "paperbreak/camera/frame.hpp"
#include "paperbreak/common/result.hpp"
#include "paperbreak/common/threading.hpp"
#include "paperbreak/event/event_window.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace paperbreak::event
{

inline constexpr std::size_t key_frame_reason_count = 7U;
inline constexpr std::size_t key_frame_default_job_capacity = 32U;

enum class KeyFrameReason
{
    normal_reference,
    earliest_abnormal,
    candidate_trigger,
    maximum_change,
    highest_confidence,
    formal_confirmation,
    post_event_state,
};

[[nodiscard]] std::string_view to_string(KeyFrameReason reason) noexcept;

struct KeyFrameReference final
{
    std::string camera_id;
    std::uint64_t sequence_number{};
    bool operator==(const KeyFrameReference&) const = default;
};

/// Per-frame evidence supplied by the ordered detection branch.
struct KeyFrameAnalysis final
{
    KeyFrameReference frame;
    bool abnormal{};
    double change_score{};
    double confidence{};
};

struct KeyFrameSelectionContext final
{
    std::vector<KeyFrameAnalysis> analyses;
    std::optional<KeyFrameReference> confirmation_frame;
};

struct KeyFrameSelectorConfig final
{
    std::size_t maximum_window_frames{65536U};
    std::size_t maximum_analysis_records{65536U};
};

struct KeyFrameDescriptor final
{
    std::string camera_id;
    std::uint64_t camera_frame_number{};
    std::uint64_t sequence_number{};
    camera::MonotonicTime monotonic_time;
    camera::WallClockTime wall_clock_time;
    camera::FrameGeometry geometry;
    camera::PixelFormat pixel_format{camera::PixelFormat::mono8};
    std::vector<KeyFrameReason> reasons;
};

struct SelectedKeyFrame final
{
    KeyFrameDescriptor descriptor;
    camera::FrameView frame;
};

struct KeyFrameSelectionResult final
{
    std::string event_id;
    std::vector<SelectedKeyFrame> frames;
    std::vector<KeyFrameReason> missing_reasons;
    bool complete{};
};

class IKeyFrameSelector
{
  public:
    virtual ~IKeyFrameSelector() = default;

    IKeyFrameSelector() = default;
    IKeyFrameSelector(const IKeyFrameSelector&) = delete;
    IKeyFrameSelector& operator=(const IKeyFrameSelector&) = delete;
    IKeyFrameSelector(IKeyFrameSelector&&) = delete;
    IKeyFrameSelector& operator=(IKeyFrameSelector&&) = delete;

    [[nodiscard]] virtual Result<KeyFrameSelectionResult> select(
        const FrozenEventWindow& event, const KeyFrameSelectionContext& context) const = 0;
};

class KeyFrameSelector final : public IKeyFrameSelector
{
  public:
    explicit KeyFrameSelector(KeyFrameSelectorConfig config = {});

    [[nodiscard]] Result<KeyFrameSelectionResult> select(
        const FrozenEventWindow& event, const KeyFrameSelectionContext& context) const override;

  private:
    KeyFrameSelectorConfig config_;
};

struct KeyFrameJpegEncodeOptions final
{
    std::uint32_t jpeg_quality{90U};
    std::uint32_t maximum_dimension{16384U};
    std::size_t maximum_input_bytes{128U * 1024U * 1024U};
    std::size_t maximum_jpeg_bytes{32U * 1024U * 1024U};
};

class IKeyFrameJpegEncoder
{
  public:
    virtual ~IKeyFrameJpegEncoder() = default;

    IKeyFrameJpegEncoder() = default;
    IKeyFrameJpegEncoder(const IKeyFrameJpegEncoder&) = delete;
    IKeyFrameJpegEncoder& operator=(const IKeyFrameJpegEncoder&) = delete;
    IKeyFrameJpegEncoder(IKeyFrameJpegEncoder&&) = delete;
    IKeyFrameJpegEncoder& operator=(IKeyFrameJpegEncoder&&) = delete;

    [[nodiscard]] virtual Result<std::vector<std::byte>> encode(
        const camera::FrameView& frame, const KeyFrameJpegEncodeOptions& options) = 0;
};

struct KeyFrameEncodingResult final
{
    std::string event_id;
    KeyFrameDescriptor descriptor;
    std::vector<std::byte> jpeg;
    std::optional<Error> error;
};

using KeyFrameEncodingCallback = std::function<void(KeyFrameEncodingResult result)>;

struct KeyFrameJpegRuntimeOptions final
{
    std::size_t job_capacity{key_frame_default_job_capacity};
    KeyFrameJpegEncodeOptions encoding;
    ThreadRegistrationFactory register_thread;
    DebugDiagnosticSink diagnostics;
};

struct KeyFrameJpegRuntimeSnapshot final
{
    bool started{};
    bool accepting{};
    std::size_t depth{};
    std::size_t capacity{};
    std::size_t high_watermark{};
    std::uint64_t submitted{};
    std::uint64_t completed{};
    std::uint64_t rejected{};
    std::uint64_t encoding_failures{};
    std::uint64_t callback_failures{};
};

/// Bounded single-worker JPEG branch. Submission never waits for queue space or encoding.
class KeyFrameJpegRuntime final
{
  public:
    class ConstructionKey final
    {
      public:
        ConstructionKey(const ConstructionKey&) = default;

      private:
        friend class KeyFrameJpegRuntime;
        ConstructionKey() = default;
    };

    [[nodiscard]] static Result<std::unique_ptr<KeyFrameJpegRuntime>> create(
        std::unique_ptr<IKeyFrameJpegEncoder> encoder, KeyFrameEncodingCallback callback,
        KeyFrameJpegRuntimeOptions options = {});

    KeyFrameJpegRuntime(ConstructionKey, std::unique_ptr<IKeyFrameJpegEncoder> encoder,
                        KeyFrameEncodingCallback callback, KeyFrameJpegRuntimeOptions options);
    ~KeyFrameJpegRuntime();
    KeyFrameJpegRuntime(const KeyFrameJpegRuntime&) = delete;
    KeyFrameJpegRuntime& operator=(const KeyFrameJpegRuntime&) = delete;
    KeyFrameJpegRuntime(KeyFrameJpegRuntime&&) = delete;
    KeyFrameJpegRuntime& operator=(KeyFrameJpegRuntime&&) = delete;

    [[nodiscard]] Result<void> start();
    [[nodiscard]] Result<void> submit(const KeyFrameSelectionResult& selection);
    void request_stop() noexcept;
    [[nodiscard]] Result<void> join(camera::MonotonicTime deadline);
    [[nodiscard]] KeyFrameJpegRuntimeSnapshot snapshot() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace paperbreak::event
