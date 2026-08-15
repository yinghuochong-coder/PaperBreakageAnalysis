#include "paperbreak/time/time_model.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>

namespace
{
using namespace paperbreak;
using namespace paperbreak::time;

ClockModelSnapshot model(const std::uint64_t revision = 7U)
{
    return {.model_revision = revision,
            .camera_id = "CAM01",
            .clock_source = ClockSource::offset_model,
            .sync_state = SyncState::degraded,
            .anchor_monotonic_ns = 10'000,
            .anchor_utc_ns = 1'000'000'000'000,
            .anchor_camera_ticks = 1'000U,
            .camera_timestamp_frequency_hz = 1'000U,
            .offset_ns = 42,
            .uncertainty_ns = 500'000,
            .maximum_observed_offset_ns = 250'000,
            .valid_from_monotonic_ns = 9'000,
            .last_synchronized_utc_ns = 999'999'000'000,
            .grandmaster_identity = std::nullopt,
            .last_error_code = "TIME_SYNC_DEGRADED"};
}
} // namespace

TEST(TimeModelEnums, PreserveFrozenNumericAndStringValues)
{
    EXPECT_EQ(static_cast<std::uint8_t>(ClockSource::unknown), 0U);
    EXPECT_EQ(static_cast<std::uint8_t>(ClockSource::receive_clock), 5U);
    EXPECT_EQ(static_cast<std::uint8_t>(SyncState::unknown), 0U);
    EXPECT_EQ(static_cast<std::uint8_t>(SyncState::unsynced), 4U);
    EXPECT_EQ(clock_source_name(ClockSource::ptp_hardware), "PTP_HARDWARE");
    EXPECT_EQ(clock_source_name(ClockSource::offset_model), "OFFSET_MODEL");
    EXPECT_EQ(sync_state_name(SyncState::syncing), "SYNCING");
    EXPECT_EQ(sync_state_name(SyncState::unsynced), "UNSYNCED");
    EXPECT_TRUE(clock_source_name(static_cast<ClockSource>(255U)).empty());
    EXPECT_TRUE(sync_state_name(static_cast<SyncState>(255U)).empty());
}

TEST(TimeModelValidation, RejectsUnpairedOrInconsistentFrameFields)
{
    FrameTimeMetadata metadata{.camera_timestamp_ticks = 1U};
    auto result = validate_frame_time_metadata(metadata);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().business_code, "TIME_MODEL_INVALID");

    metadata = {.camera_timestamp_ticks = 1U, .camera_timestamp_frequency_hz = 0U};
    EXPECT_FALSE(validate_frame_time_metadata(metadata));

    metadata = {.corrected_capture_utc_ns = 100,
                .clock_source = ClockSource::offset_model,
                .uncertainty_ns = 1,
                .sync_state = SyncState::degraded,
                .clock_model_revision = 0U};
    EXPECT_FALSE(validate_frame_time_metadata(metadata));

    metadata = {.uncertainty_ns = -1, .sync_state = SyncState::unsynced};
    EXPECT_FALSE(validate_frame_time_metadata(metadata));

    metadata = {.clock_source = ClockSource::offset_model,
                .sync_state = SyncState::degraded,
                .clock_model_revision = 8U};
    EXPECT_FALSE(validate_frame_time_metadata(metadata));
}

TEST(TimeModelValidation, RejectsInvalidPublishedModels)
{
    auto snapshot = model();
    EXPECT_TRUE(validate_clock_model_snapshot(snapshot));

    snapshot.model_revision = 0U;
    EXPECT_FALSE(validate_clock_model_snapshot(snapshot));
    snapshot = model();
    snapshot.camera_timestamp_frequency_hz.reset();
    EXPECT_FALSE(validate_clock_model_snapshot(snapshot));
    snapshot = model();
    snapshot.uncertainty_ns = -1;
    EXPECT_FALSE(validate_clock_model_snapshot(snapshot));
    snapshot = model();
    snapshot.sync_state = SyncState::unsynced;
    EXPECT_FALSE(validate_clock_model_snapshot(snapshot));
    snapshot = model();
    snapshot.sync_state = SyncState::synced;
    EXPECT_FALSE(validate_clock_model_snapshot(snapshot));
}

TEST(TimeModelMapping, PreservesRawAndReceiveEvidenceWithCheckedIntegerCorrection)
{
    auto snapshot = std::make_shared<const ClockModelSnapshot>(model());
    const auto result =
        build_frame_time_metadata(1'123U, 1'000U, 12'000, 1'000'000'001'000, snapshot);

    EXPECT_EQ(result.status, FrameTimeBuildStatus::corrected);
    EXPECT_EQ(result.metadata.camera_timestamp_ticks, 1'123U);
    EXPECT_EQ(result.metadata.camera_timestamp_frequency_hz, 1'000U);
    EXPECT_EQ(result.metadata.received_monotonic_ns, 12'000);
    EXPECT_EQ(result.metadata.received_utc_ns, 1'000'000'001'000);
    EXPECT_EQ(result.metadata.corrected_capture_utc_ns, 1'000'123'000'000);
    EXPECT_EQ(result.metadata.clock_source, ClockSource::offset_model);
    EXPECT_EQ(result.metadata.clock_offset_ns, 42);
    EXPECT_EQ(result.metadata.uncertainty_ns, 500'000);
    EXPECT_EQ(result.metadata.sync_state, SyncState::degraded);
    EXPECT_EQ(result.metadata.clock_model_revision, 7U);
    EXPECT_TRUE(validate_frame_time_metadata(result.metadata));

    const auto before_anchor =
        build_frame_time_metadata(999U, 1'000U, 12'001, 1'000'000'001'001, snapshot);
    EXPECT_EQ(before_anchor.metadata.corrected_capture_utc_ns, 999'999'000'000);
}

TEST(TimeModelMapping, UsesIntegerFractionWithoutDoubleOrWideIntegerAssumptions)
{
    auto snapshot = model();
    snapshot.anchor_camera_ticks = 0U;
    snapshot.camera_timestamp_frequency_hz = 3U;
    auto published = std::make_shared<const ClockModelSnapshot>(snapshot);

    const auto one_third = build_frame_time_metadata(1U, 3U, 12'000, 1'000'000'001'000, published);
    const auto two_thirds = build_frame_time_metadata(2U, 3U, 12'000, 1'000'000'001'000, published);
    EXPECT_EQ(one_third.metadata.corrected_capture_utc_ns, 1'000'333'333'333);
    EXPECT_EQ(two_thirds.metadata.corrected_capture_utc_ns, 1'000'666'666'666);
}

TEST(TimeModelMapping, MissingTicksAndInvalidInputsRemainExplicitlyUnsynced)
{
    auto snapshot = std::make_shared<const ClockModelSnapshot>(model());
    const auto missing = build_frame_time_metadata({}, {}, 12'000, 1'000'000'001'000, snapshot);
    EXPECT_EQ(missing.status, FrameTimeBuildStatus::missing_camera_timestamp);
    EXPECT_FALSE(missing.metadata.corrected_capture_utc_ns);
    EXPECT_EQ(missing.metadata.sync_state, SyncState::unsynced);
    EXPECT_EQ(missing.metadata.clock_source, ClockSource::unknown);
    EXPECT_EQ(missing.metadata.clock_model_revision, 0U);
    EXPECT_EQ(missing.metadata.received_utc_ns, 1'000'000'001'000);

    const auto unpaired = build_frame_time_metadata(1U, {}, 12'000, 1'000'000'001'000, snapshot);
    EXPECT_EQ(unpaired.status, FrameTimeBuildStatus::invalid_raw_timestamp);
    EXPECT_FALSE(unpaired.metadata.camera_timestamp_ticks);
    EXPECT_FALSE(unpaired.metadata.camera_timestamp_frequency_hz);

    const auto mismatch =
        build_frame_time_metadata(1'001U, 2'000U, 12'000, 1'000'000'001'000, snapshot);
    EXPECT_EQ(mismatch.status, FrameTimeBuildStatus::frequency_mismatch);
    EXPECT_EQ(mismatch.metadata.camera_timestamp_ticks, 1'001U);
    EXPECT_FALSE(mismatch.metadata.corrected_capture_utc_ns);
}

TEST(TimeModelMapping, RejectsPositiveAndNegativeArithmeticOverflowWithoutWrapping)
{
    auto positive = model();
    positive.anchor_utc_ns = std::numeric_limits<std::int64_t>::max() - 10;
    auto result = build_frame_time_metadata(1'001U, 1'000U, 12'000, 0,
                                            std::make_shared<const ClockModelSnapshot>(positive));
    EXPECT_EQ(result.status, FrameTimeBuildStatus::arithmetic_overflow);
    EXPECT_FALSE(result.metadata.corrected_capture_utc_ns);

    auto negative = model();
    negative.anchor_utc_ns = std::numeric_limits<std::int64_t>::min() + 10;
    result = build_frame_time_metadata(999U, 1'000U, 12'000, 0,
                                       std::make_shared<const ClockModelSnapshot>(negative));
    EXPECT_EQ(result.status, FrameTimeBuildStatus::arithmetic_overflow);
    EXPECT_FALSE(result.metadata.corrected_capture_utc_ns);

    auto enormous_delta = model();
    enormous_delta.anchor_camera_ticks = 0U;
    enormous_delta.camera_timestamp_frequency_hz = 1U;
    result = build_frame_time_metadata(std::numeric_limits<std::uint64_t>::max(), 1U, 12'000, 0,
                                       std::make_shared<const ClockModelSnapshot>(enormous_delta));
    EXPECT_EQ(result.status, FrameTimeBuildStatus::arithmetic_overflow);
}

TEST(TimeModelPublication, ModelSwitchDoesNotMutatePreviouslyBuiltFrame)
{
    ImmutableClockModelStore store;
    store.publish(std::make_shared<const ClockModelSnapshot>(model(7U)));
    const auto loaded_first = store.load();
    const auto historical =
        build_frame_time_metadata(1'001U, 1'000U, 12'000, 0, loaded_first).metadata;

    auto next = model(8U);
    next.anchor_utc_ns += 5'000'000'000;
    store.publish(std::make_shared<const ClockModelSnapshot>(next));
    const auto current =
        build_frame_time_metadata(1'001U, 1'000U, 12'000, 0, store.load()).metadata;

    EXPECT_EQ(historical.clock_model_revision, 7U);
    EXPECT_EQ(historical.corrected_capture_utc_ns, 1'000'001'000'000);
    EXPECT_EQ(current.clock_model_revision, 8U);
    EXPECT_EQ(current.corrected_capture_utc_ns, 1'005'001'000'000);
    EXPECT_EQ(loaded_first->model_revision, 7U);

    store.clear();
    EXPECT_FALSE(store.load());
    EXPECT_EQ(historical.clock_model_revision, 7U);
}

TEST(TimeModelClockConversion, ConvertsRepresentableClockPointsToSignedNanoseconds)
{
    EXPECT_EQ(monotonic_time_to_nanoseconds(
                  std::chrono::steady_clock::time_point{std::chrono::milliseconds{123}}),
              123'000'000);
    EXPECT_EQ(utc_time_to_nanoseconds(
                  std::chrono::system_clock::time_point{std::chrono::milliseconds{-456}}),
              -456'000'000);
}

TEST(TimeModelReceiveClock, UsesReceiveUtcAsExplicitDegradedCorrection)
{
    auto snapshot = model(9U);
    snapshot.clock_source = ClockSource::receive_clock;
    snapshot.sync_state = SyncState::degraded;
    snapshot.anchor_camera_ticks.reset();
    snapshot.camera_timestamp_frequency_hz.reset();
    snapshot.offset_ns.reset();
    snapshot.uncertainty_ns = 50'000'000;
    auto published = std::make_shared<const ClockModelSnapshot>(snapshot);

    const auto result = build_frame_time_metadata({}, {}, 12'000, 5'000'000'000, published);
    EXPECT_EQ(result.status, FrameTimeBuildStatus::corrected);
    EXPECT_EQ(result.metadata.corrected_capture_utc_ns, 5'000'000'000);
    EXPECT_EQ(result.metadata.clock_source, ClockSource::receive_clock);
    EXPECT_EQ(result.metadata.sync_state, SyncState::degraded);
    EXPECT_EQ(result.metadata.clock_model_revision, 9U);
    EXPECT_TRUE(validate_frame_time_metadata(result.metadata));
}

TEST(TimeModelHostMapping, MapsBothDirectionsAndRetainsTheExactModel)
{
    auto snapshot = model(11U);
    snapshot.camera_id.reset();
    snapshot.anchor_monotonic_ns = 1'000;
    snapshot.anchor_utc_ns = 10'000;
    snapshot.valid_from_monotonic_ns = 900;
    snapshot.anchor_camera_ticks.reset();
    snapshot.camera_timestamp_frequency_hz.reset();
    auto published = std::make_shared<const ClockModelSnapshot>(snapshot);

    const auto utc = map_monotonic_to_utc(1'250, published);
    ASSERT_TRUE(utc);
    EXPECT_EQ(utc.value().mapped_time_ns, 10'250);
    EXPECT_EQ(utc.value().model.get(), published.get());

    const auto monotonic = map_utc_to_monotonic(10'500, published);
    ASSERT_TRUE(monotonic);
    EXPECT_EQ(monotonic.value().mapped_time_ns, 1'500);
    EXPECT_EQ(monotonic.value().model->model_revision, 11U);

    const auto status = build_clock_sync_snapshot(published, 1'400);
    EXPECT_TRUE(status.available);
    EXPECT_EQ(status.current_utc_ns, 10'400);
    EXPECT_TRUE(status.offset_available);
    EXPECT_TRUE(status.uncertainty_available);
    EXPECT_FALSE(status.grandmaster_available);
    EXPECT_EQ(status.model_revision, 11U);
}

TEST(TimeModelHostMapping, RejectsUncoveredAndOverflowingTargets)
{
    auto snapshot = model(12U);
    snapshot.camera_id.reset();
    snapshot.anchor_camera_ticks.reset();
    snapshot.camera_timestamp_frequency_hz.reset();
    snapshot.anchor_monotonic_ns = 100;
    snapshot.anchor_utc_ns = std::numeric_limits<std::int64_t>::max() - 5;
    snapshot.valid_from_monotonic_ns = 100;
    auto published = std::make_shared<const ClockModelSnapshot>(snapshot);

    const auto before = map_monotonic_to_utc(99, published);
    ASSERT_FALSE(before);
    EXPECT_EQ(before.error().business_code, "TIME_MAPPING_UNAVAILABLE");
    EXPECT_FALSE(map_monotonic_to_utc(106, published));
    EXPECT_FALSE(map_utc_to_monotonic(std::numeric_limits<std::int64_t>::min(), published));

    const auto unavailable = build_clock_sync_snapshot({}, 100);
    EXPECT_FALSE(unavailable.available);
    EXPECT_EQ(unavailable.clock_source, ClockSource::unknown);
    EXPECT_EQ(unavailable.sync_state, SyncState::unknown);
}
