#include "paperbreak/event/key_frame.hpp"
#include "paperbreak/event/key_frame_opencv.hpp"

#include <gtest/gtest.h>

#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
using namespace std::chrono_literals;
using paperbreak::Error;
using paperbreak::Result;
using paperbreak::Severity;
using paperbreak::algorithm::TriggerResult;
using paperbreak::algorithm::TriggerSource;
using paperbreak::camera::FrameBuffer;
using paperbreak::camera::FrameGeometry;
using paperbreak::camera::FramePacket;
using paperbreak::camera::FrameView;
using paperbreak::camera::MonotonicTime;
using paperbreak::camera::PixelFormat;
using paperbreak::camera::WallClockTime;
using paperbreak::event::EventWindowTrigger;
using paperbreak::event::FrozenCameraWindow;
using paperbreak::event::FrozenEventWindow;
using paperbreak::event::IKeyFrameJpegEncoder;
using paperbreak::event::KeyFrameAnalysis;
using paperbreak::event::KeyFrameDescriptor;
using paperbreak::event::KeyFrameEncodingResult;
using paperbreak::event::KeyFrameJpegEncodeOptions;
using paperbreak::event::KeyFrameJpegRuntime;
using paperbreak::event::KeyFrameJpegRuntimeOptions;
using paperbreak::event::KeyFrameReason;
using paperbreak::event::KeyFrameReference;
using paperbreak::event::KeyFrameSelectionContext;
using paperbreak::event::KeyFrameSelectionResult;
using paperbreak::event::KeyFrameSelector;
using paperbreak::event::SelectedKeyFrame;

FrameView make_frame(const std::string& camera_id, const std::uint64_t sequence,
                     const std::chrono::milliseconds time,
                     const PixelFormat format = PixelFormat::mono8, const std::uint32_t width = 8U,
                     const std::uint32_t height = 6U)
{
    const std::size_t bytes_per_pixel =
        format == PixelFormat::mono10 || format == PixelFormat::mono12 ? 2U : 1U;
    const auto stride =
        static_cast<std::uint32_t>(static_cast<std::size_t>(width) * bytes_per_pixel);
    auto buffer = std::make_shared<FrameBuffer>(static_cast<std::size_t>(stride) * height);
    auto bytes = buffer->writable_bytes();
    if (bytes_per_pixel == 1U)
    {
        for (std::size_t index = 0U; index < bytes.size(); ++index)
            bytes[index] = static_cast<std::byte>((index * 17U + sequence) % 256U);
    }
    else
    {
        const std::uint16_t maximum = format == PixelFormat::mono10 ? 1023U : 4095U;
        for (std::size_t index = 0U; index < bytes.size() / 2U; ++index)
        {
            const auto value = static_cast<std::uint16_t>((index * 37U + sequence) % maximum);
            std::memcpy(bytes.data() + index * 2U, &value, sizeof(value));
        }
    }
    EXPECT_TRUE(buffer->set_size(static_cast<std::size_t>(stride) * height));
    FramePacket packet{.camera_id = camera_id,
                       .camera_frame_number = 1000U + sequence,
                       .sequence_number = sequence,
                       .received_monotonic_time = MonotonicTime{time},
                       .received_wall_clock_time = WallClockTime{1h + time},
                       .geometry = {.width = width, .height = height, .stride = stride},
                       .pixel_format = format,
                       .buffer = std::move(buffer)};
    auto frame = paperbreak::camera::make_frame_view(packet);
    EXPECT_TRUE(frame);
    return std::move(frame).value();
}

TriggerResult trigger(const std::string& camera_id, const std::uint64_t sequence,
                      const std::chrono::milliseconds time)
{
    return {.triggered = true,
            .trigger_source = TriggerSource::manual_test,
            .camera_id = camera_id,
            .sequence_number = sequence,
            .camera_frame_number = 1000U + sequence,
            .monotonic_time = MonotonicTime{time},
            .wall_clock_time = WallClockTime{1h + time},
            .mean_grayscale = 80.0,
            .mean_grayscale_change = 20.0,
            .paper_ratio = 0.3,
            .reason = "test"};
}

FrozenEventWindow event_window(std::vector<FrameView> frames,
                               const std::uint64_t trigger_sequence = 3U,
                               const std::chrono::milliseconds trigger_time = 20ms)
{
    return {.event_id = "0198-event",
            .version = 1U,
            .requested_start = MonotonicTime{0ms},
            .requested_end = MonotonicTime{40ms},
            .closed_monotonic_time = MonotonicTime{50ms},
            .display_wall_clock_time = WallClockTime{1h},
            .triggers = {{.source_event_id = "source",
                          .trigger = trigger("CAM01", trigger_sequence, trigger_time)}},
            .camera_windows = {{.camera_id = "CAM01",
                                .requested_start = MonotonicTime{0ms},
                                .requested_end = MonotonicTime{40ms},
                                .available_start = MonotonicTime{0ms},
                                .available_end = MonotonicTime{40ms},
                                .first_sequence_number = 1U,
                                .last_sequence_number = frames.back().sequence_number(),
                                .frames = std::move(frames),
                                .complete = true}},
            .complete = true};
}

const SelectedKeyFrame* selected_sequence(const KeyFrameSelectionResult& result,
                                          const std::uint64_t sequence)
{
    const auto iterator = std::ranges::find_if(result.frames, [sequence](const auto& frame) {
        return frame.descriptor.sequence_number == sequence;
    });
    return iterator == result.frames.end() ? nullptr : &*iterator;
}

bool has_reason(const SelectedKeyFrame& frame, const KeyFrameReason reason)
{
    return std::ranges::find(frame.descriptor.reasons, reason) != frame.descriptor.reasons.end();
}

SelectedKeyFrame selected_frame(FrameView frame, std::vector<KeyFrameReason> reasons)
{
    KeyFrameDescriptor descriptor{.camera_id = frame.camera_id(),
                                  .camera_frame_number = frame.camera_frame_number(),
                                  .sequence_number = frame.sequence_number(),
                                  .monotonic_time = frame.received_monotonic_time(),
                                  .wall_clock_time = frame.received_wall_clock_time(),
                                  .geometry = frame.geometry(),
                                  .pixel_format = frame.pixel_format(),
                                  .reasons = std::move(reasons)};
    return {.descriptor = std::move(descriptor), .frame = std::move(frame)};
}

class ScriptedEncoder final : public IKeyFrameJpegEncoder
{
  public:
    explicit ScriptedEncoder(const bool fail_first = false) : fail_first_(fail_first) {}

    Result<std::vector<std::byte>> encode(const FrameView&,
                                          const KeyFrameJpegEncodeOptions&) override
    {
        std::scoped_lock lock{mutex_};
        ++calls_;
        if (fail_first_ && calls_ == 1U)
        {
            return Result<std::vector<std::byte>>::failure(
                paperbreak::make_error("EVENT_KEYFRAME_ENCODE_FAILED", Severity::error, "injected",
                                       "event", "keyframe.test", false));
        }
        return Result<std::vector<std::byte>>::success({std::byte{0x01}, std::byte{0x02}});
    }

  private:
    std::mutex mutex_;
    std::uint64_t calls_{};
    bool fail_first_{};
};

TEST(KeyFrameSelectorRoles, SelectsSevenRolesAndMergesSharedPhysicalFrames)
{
    auto event = event_window({make_frame("CAM01", 1U, 0ms), make_frame("CAM01", 2U, 10ms),
                               make_frame("CAM01", 3U, 20ms), make_frame("CAM01", 4U, 30ms)});
    KeyFrameSelectionContext context{.analyses = {{{"CAM01", 1U}, false, 0.0, 0.1},
                                                  {{"CAM01", 2U}, true, 0.4, 0.6},
                                                  {{"CAM01", 3U}, true, 0.9, 0.95},
                                                  {{"CAM01", 4U}, true, 0.2, 0.7}},
                                     .confirmation_frame = KeyFrameReference{"CAM01", 3U}};

    auto result = KeyFrameSelector{}.select(event, context);
    ASSERT_TRUE(result);
    EXPECT_TRUE(result.value().complete);
    EXPECT_TRUE(result.value().missing_reasons.empty());
    ASSERT_EQ(result.value().frames.size(), 4U);

    const auto* normal = selected_sequence(result.value(), 1U);
    const auto* abnormal = selected_sequence(result.value(), 2U);
    const auto* trigger_frame = selected_sequence(result.value(), 3U);
    const auto* post = selected_sequence(result.value(), 4U);
    ASSERT_NE(normal, nullptr);
    ASSERT_NE(abnormal, nullptr);
    ASSERT_NE(trigger_frame, nullptr);
    ASSERT_NE(post, nullptr);
    EXPECT_TRUE(has_reason(*normal, KeyFrameReason::normal_reference));
    EXPECT_TRUE(has_reason(*abnormal, KeyFrameReason::earliest_abnormal));
    EXPECT_TRUE(has_reason(*trigger_frame, KeyFrameReason::candidate_trigger));
    EXPECT_TRUE(has_reason(*trigger_frame, KeyFrameReason::maximum_change));
    EXPECT_TRUE(has_reason(*trigger_frame, KeyFrameReason::highest_confidence));
    EXPECT_TRUE(has_reason(*trigger_frame, KeyFrameReason::formal_confirmation));
    EXPECT_TRUE(has_reason(*post, KeyFrameReason::post_event_state));
    EXPECT_EQ(trigger_frame->descriptor.camera_frame_number, 1003U);
    EXPECT_EQ(trigger_frame->descriptor.monotonic_time, MonotonicTime{20ms});
    EXPECT_EQ(trigger_frame->descriptor.wall_clock_time, WallClockTime{1h + 20ms});
    EXPECT_EQ(paperbreak::event::to_string(KeyFrameReason::normal_reference), "NormalReference");
}

TEST(KeyFrameSelectorDeterminism, UsesStableTiesAndReportsUnavailableRoles)
{
    auto event = event_window({make_frame("CAM01", 1U, 0ms), make_frame("CAM01", 2U, 10ms),
                               make_frame("CAM01", 3U, 20ms)},
                              3U, 20ms);
    KeyFrameSelectionContext context{.analyses = {{{"CAM01", 1U}, true, 0.8, 0.7},
                                                  {{"CAM01", 2U}, true, 0.8, 0.7},
                                                  {{"CAM01", 3U}, true, 0.1, 0.2}}};

    auto result = KeyFrameSelector{}.select(event, context);
    ASSERT_TRUE(result);
    EXPECT_FALSE(result.value().complete);
    const auto* first = selected_sequence(result.value(), 1U);
    ASSERT_NE(first, nullptr);
    EXPECT_TRUE(has_reason(*first, KeyFrameReason::earliest_abnormal));
    EXPECT_TRUE(has_reason(*first, KeyFrameReason::maximum_change));
    EXPECT_TRUE(has_reason(*first, KeyFrameReason::highest_confidence));
    EXPECT_EQ(result.value().missing_reasons,
              (std::vector{KeyFrameReason::normal_reference, KeyFrameReason::formal_confirmation,
                           KeyFrameReason::post_event_state}));
}

TEST(KeyFrameSelectorValidation, RejectsUnknownDuplicateAndInvalidEvidence)
{
    auto event = event_window({make_frame("CAM01", 1U, 0ms), make_frame("CAM01", 2U, 10ms),
                               make_frame("CAM01", 3U, 20ms), make_frame("CAM01", 4U, 30ms)});
    KeyFrameSelectionContext unknown{.analyses = {{{"CAM01", 99U}, true, 1.0, 0.5}}};
    auto unknown_result = KeyFrameSelector{}.select(event, unknown);
    ASSERT_FALSE(unknown_result);
    EXPECT_EQ(unknown_result.error().business_code, "EVENT_KEYFRAME_SELECTION_FAILED");

    KeyFrameSelectionContext duplicate{
        .analyses = {{{"CAM01", 1U}, false, 0.0, 0.1}, {{"CAM01", 1U}, true, 1.0, 0.9}}};
    EXPECT_FALSE(KeyFrameSelector{}.select(event, duplicate));

    KeyFrameSelectionContext invalid{.analyses = {{{"CAM01", 1U}, false, -1.0, 1.1}},
                                     .confirmation_frame = KeyFrameReference{"CAM02", 1U}};
    EXPECT_FALSE(KeyFrameSelector{}.select(event, invalid));
}

TEST(KeyFrameJpegQueue, RejectsAnEventAtomicallyWhenCapacityCannotFit)
{
    auto runtime_result = KeyFrameJpegRuntime::create(
        std::make_unique<ScriptedEncoder>(), [](KeyFrameEncodingResult) {},
        KeyFrameJpegRuntimeOptions{.job_capacity = 1U});
    ASSERT_TRUE(runtime_result);
    auto runtime = std::move(runtime_result).value();
    ASSERT_TRUE(runtime->start());

    KeyFrameSelectionResult too_many{
        .event_id = "event-a",
        .frames = {
            selected_frame(make_frame("CAM01", 1U, 0ms), {KeyFrameReason::normal_reference}),
            selected_frame(make_frame("CAM01", 2U, 10ms), {KeyFrameReason::earliest_abnormal})}};
    auto rejected = runtime->submit(too_many);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().business_code, "EVENT_KEYFRAME_QUEUE_FULL");
    EXPECT_EQ(runtime->snapshot().submitted, 0U);
    EXPECT_EQ(runtime->snapshot().rejected, 2U);

    KeyFrameSelectionResult accepted{
        .event_id = "event-b",
        .frames = {
            selected_frame(make_frame("CAM01", 3U, 20ms), {KeyFrameReason::candidate_trigger})}};
    ASSERT_TRUE(runtime->submit(accepted));
    runtime->request_stop();
    EXPECT_TRUE(runtime->join(MonotonicTime::clock::now() + 2s));
    const auto snapshot = runtime->snapshot();
    EXPECT_EQ(snapshot.submitted, 1U);
    EXPECT_EQ(snapshot.completed, 1U);
    EXPECT_EQ(snapshot.high_watermark, 1U);
    EXPECT_EQ(snapshot.depth, 0U);
}

TEST(KeyFrameJpegFailures, IsolatesEncoderAndCallbackFailuresAndDrainsAcceptedJobs)
{
    std::mutex result_mutex;
    std::vector<KeyFrameEncodingResult> successes;
    auto runtime_result = KeyFrameJpegRuntime::create(
        std::make_unique<ScriptedEncoder>(true),
        [&](KeyFrameEncodingResult result) {
            if (result.error)
                throw std::runtime_error{"injected callback failure"};
            std::scoped_lock lock{result_mutex};
            successes.push_back(std::move(result));
        },
        KeyFrameJpegRuntimeOptions{.job_capacity = 2U});
    ASSERT_TRUE(runtime_result);
    auto runtime = std::move(runtime_result).value();
    ASSERT_TRUE(runtime->start());

    KeyFrameSelectionResult selection{
        .event_id = "event-a",
        .frames = {
            selected_frame(make_frame("CAM01", 1U, 0ms), {KeyFrameReason::normal_reference}),
            selected_frame(make_frame("CAM01", 2U, 10ms), {KeyFrameReason::earliest_abnormal})}};
    ASSERT_TRUE(runtime->submit(selection));
    runtime->request_stop();
    ASSERT_TRUE(runtime->join(MonotonicTime::clock::now() + 2s));
    const auto snapshot = runtime->snapshot();
    EXPECT_EQ(snapshot.completed, 2U);
    EXPECT_EQ(snapshot.encoding_failures, 1U);
    EXPECT_EQ(snapshot.callback_failures, 1U);
    std::scoped_lock lock{result_mutex};
    ASSERT_EQ(successes.size(), 1U);
    EXPECT_EQ(successes.front().descriptor.sequence_number, 2U);
    EXPECT_EQ(successes.front().jpeg.size(), 2U);

    auto after_stop = runtime->submit(selection);
    ASSERT_FALSE(after_stop);
    EXPECT_EQ(after_stop.error().business_code, "EVENT_KEYFRAME_QUEUE_FULL");
}

TEST(KeyFrameJpegOpenCv, EncodesAllApprovedPixelFormatsOnTheWorker)
{
    std::mutex result_mutex;
    std::vector<KeyFrameEncodingResult> results;
    auto runtime_result = KeyFrameJpegRuntime::create(
        paperbreak::event::make_opencv_key_frame_jpeg_encoder(),
        [&](KeyFrameEncodingResult result) {
            std::scoped_lock lock{result_mutex};
            results.push_back(std::move(result));
        },
        KeyFrameJpegRuntimeOptions{.job_capacity = 4U});
    ASSERT_TRUE(runtime_result);
    auto runtime = std::move(runtime_result).value();
    ASSERT_TRUE(runtime->start());

    KeyFrameSelectionResult selection{
        .event_id = "event-formats",
        .frames = {selected_frame(make_frame("CAM01", 1U, 0ms, PixelFormat::mono8),
                                  {KeyFrameReason::normal_reference}),
                   selected_frame(make_frame("CAM01", 2U, 10ms, PixelFormat::mono10),
                                  {KeyFrameReason::earliest_abnormal}),
                   selected_frame(make_frame("CAM01", 3U, 20ms, PixelFormat::mono12),
                                  {KeyFrameReason::candidate_trigger}),
                   selected_frame(make_frame("CAM01", 4U, 30ms, PixelFormat::bayer_rg8),
                                  {KeyFrameReason::post_event_state})}};
    ASSERT_TRUE(runtime->submit(selection));
    runtime->request_stop();
    ASSERT_TRUE(runtime->join(MonotonicTime::clock::now() + 5s));

    std::scoped_lock lock{result_mutex};
    ASSERT_EQ(results.size(), 4U);
    for (const auto& result : results)
    {
        ASSERT_FALSE(result.error);
        ASSERT_FALSE(result.jpeg.empty());
        const cv::Mat encoded(1, static_cast<int>(result.jpeg.size()), CV_8UC1,
                              const_cast<std::byte*>(result.jpeg.data()));
        const cv::Mat decoded = cv::imdecode(encoded, cv::IMREAD_UNCHANGED);
        ASSERT_FALSE(decoded.empty());
        EXPECT_EQ(decoded.cols, 8);
        EXPECT_EQ(decoded.rows, 6);
    }
}

TEST(KeyFrameJpegOpenCv, ReportsOutputLimitWithoutStoppingTheWorker)
{
    std::mutex result_mutex;
    std::vector<KeyFrameEncodingResult> results;
    auto runtime_result = KeyFrameJpegRuntime::create(
        paperbreak::event::make_opencv_key_frame_jpeg_encoder(),
        [&](KeyFrameEncodingResult result) {
            std::scoped_lock lock{result_mutex};
            results.push_back(std::move(result));
        },
        KeyFrameJpegRuntimeOptions{.job_capacity = 1U, .encoding = {.maximum_jpeg_bytes = 1U}});
    ASSERT_TRUE(runtime_result);
    auto runtime = std::move(runtime_result).value();
    ASSERT_TRUE(runtime->start());
    KeyFrameSelectionResult selection{
        .event_id = "event-limit",
        .frames = {
            selected_frame(make_frame("CAM01", 1U, 0ms), {KeyFrameReason::normal_reference})}};
    ASSERT_TRUE(runtime->submit(selection));
    runtime->request_stop();
    ASSERT_TRUE(runtime->join(MonotonicTime::clock::now() + 5s));

    std::scoped_lock lock{result_mutex};
    ASSERT_EQ(results.size(), 1U);
    ASSERT_TRUE(results.front().error);
    EXPECT_EQ(results.front().error->business_code, "EVENT_KEYFRAME_ENCODE_FAILED");
    EXPECT_EQ(runtime->snapshot().encoding_failures, 1U);
}

} // namespace
