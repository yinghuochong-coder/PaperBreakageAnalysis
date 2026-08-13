#include "paperbreak/console/algorithm_metrics.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <string>

namespace
{

paperbreak::console::AlgorithmClientSnapshot valid_snapshot(std::string camera_id,
                                                            const std::uint64_t sequence)
{
    paperbreak::console::AlgorithmClientSnapshot snapshot;
    snapshot.camera_id = std::move(camera_id);
    snapshot.runtime.camera_id = snapshot.camera_id;
    snapshot.stale = false;
    snapshot.local_sample_sequence = sequence;
    snapshot.local_sample_time =
        std::chrono::system_clock::time_point{std::chrono::milliseconds{1700000000000LL}} +
        std::chrono::seconds{sequence};
    snapshot.runtime.metrics.processed_fps = static_cast<double>(sequence);
    return snapshot;
}

TEST(AlgorithmMetrics, RegistryContainsAllUniqueDocumentedMetrics)
{
    const std::set<std::string_view> expected_keys{"queueDepth",
                                                   "queueCapacity",
                                                   "queueHighWatermark",
                                                   "submittedFrames",
                                                   "processedFrames",
                                                   "skippedFrames",
                                                   "sampledSkippedFrames",
                                                   "missedProcessingSlots",
                                                   "configuredProcessingFps",
                                                   "detectorFailures",
                                                   "consecutiveDetectorFailures",
                                                   "consecutiveBacklogEvents",
                                                   "backlogActive",
                                                   "consecutiveBadBacklogWindows",
                                                   "consecutiveHealthyBacklogWindows",
                                                   "resultQueueRejected",
                                                   "rearmPending",
                                                   "rearmSuppressedResults",
                                                   "processCalls",
                                                   "lastProcessingTimeUs",
                                                   "averageProcessingTimeUs",
                                                   "maximumProcessingTimeUs",
                                                   "lastQueueWaitTimeUs",
                                                   "averageQueueWaitTimeUs",
                                                   "maximumQueueWaitTimeUs",
                                                   "lastEndToEndTimeUs",
                                                   "averageEndToEndTimeUs",
                                                   "maximumEndToEndTimeUs",
                                                   "inputFps",
                                                   "processedFps",
                                                   "skippedRatio",
                                                   "candidatesCreated",
                                                   "confirmedEvents",
                                                   "rejectedCandidates"};
    const auto descriptors = paperbreak::console::algorithm_metric_descriptors();
    ASSERT_EQ(descriptors.size(), 34U);
    std::set<std::string_view> keys;
    std::set<paperbreak::console::AlgorithmMetricGroup> groups;
    for (const auto& descriptor : descriptors)
    {
        EXPECT_FALSE(descriptor.key.empty());
        EXPECT_FALSE(descriptor.chinese_name.empty());
        EXPECT_FALSE(descriptor.unit.empty());
        EXPECT_FALSE(descriptor.definition.empty());
        EXPECT_NE(descriptor.read, nullptr);
        EXPECT_TRUE(keys.insert(descriptor.key).second) << descriptor.key;
        groups.insert(descriptor.group);
        EXPECT_FALSE(paperbreak::console::algorithm_metric_group_name(descriptor.group).empty());
    }
    EXPECT_EQ(groups.size(), 4U);
    EXPECT_EQ(keys, expected_keys);

    paperbreak::console::AlgorithmMetricValue metric_values;
    metric_values.skipped_ratio = 0.125;
    const auto skipped_ratio =
        std::ranges::find(descriptors, std::string_view{"skippedRatio"},
                          &paperbreak::console::AlgorithmMetricDescriptor::key);
    ASSERT_NE(skipped_ratio, descriptors.end());
    EXPECT_DOUBLE_EQ(
        paperbreak::console::algorithm_metric_numeric_value(
            paperbreak::console::algorithm_metric_value(*skipped_ratio, metric_values)),
        12.5);

    const auto debug = paperbreak::console::algorithm_debug_metric_descriptors();
    ASSERT_EQ(debug.size(), 11U);
    for (const auto& descriptor : debug)
    {
        EXPECT_FALSE(descriptor.chinese_name.empty());
        EXPECT_FALSE(descriptor.definition.empty());
    }
}

TEST(AlgorithmMetrics, HistoryIsBoundedIsolatedAndIgnoresDuplicateNotifications)
{
    paperbreak::console::AlgorithmMetricHistory history;
    for (std::uint64_t sequence = 1U; sequence <= 105U; ++sequence)
    {
        auto snapshot = valid_snapshot("CAM01", sequence);
        EXPECT_TRUE(history.ingest(snapshot));
        EXPECT_FALSE(history.ingest(snapshot));
    }
    EXPECT_EQ(history.size("CAM01", "processedFps"), 100U);
    const auto camera_one = history.history("CAM01", "processedFps");
    ASSERT_EQ(camera_one.size(), 100U);
    EXPECT_DOUBLE_EQ(camera_one.front().value, 6.0);
    EXPECT_DOUBLE_EQ(camera_one.back().value, 105.0);

    auto camera_two = valid_snapshot("CAM02", 106U);
    camera_two.runtime.metrics.processed_fps = 7.5;
    EXPECT_TRUE(history.ingest(camera_two));
    EXPECT_EQ(history.size("CAM02", "processedFps"), 1U);
    EXPECT_EQ(history.size("CAM01", "processedFps"), 100U);

    auto stale = valid_snapshot("CAM01", 107U);
    stale.stale = true;
    EXPECT_FALSE(history.ingest(stale));
    EXPECT_EQ(history.size("CAM01", "processedFps"), 100U);
}

TEST(AlgorithmMetrics, CsvExportsExactCurrentValuesAndEscapesPluginText)
{
    auto snapshot = valid_snapshot("CAM03", 9U);
    snapshot.runtime.metrics.submitted_frames = std::numeric_limits<std::uint64_t>::max();
    snapshot.test_result.emplace();
    snapshot.test_result->debug_metrics = {{.name = "paperRatio", .value = 0.25},
                                           {.name = "插件,\"值\"", .value = 12.5}};
    const auto path =
        std::filesystem::temp_directory_path() / "PaperBreakEdge-algorithm-metrics-test.csv";
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    const auto exported = paperbreak::console::export_algorithm_current_values_csv(
        snapshot, path,
        std::chrono::system_clock::time_point{std::chrono::milliseconds{1700000100000LL}});
    ASSERT_TRUE(exported) << exported.error().message;

    std::ifstream input{path, std::ios::binary};
    const std::string contents{std::istreambuf_iterator<char>{input},
                               std::istreambuf_iterator<char>{}};
    ASSERT_GE(contents.size(), 3U);
    EXPECT_EQ(static_cast<unsigned char>(contents[0]), 0xEFU);
    EXPECT_EQ(static_cast<unsigned char>(contents[1]), 0xBBU);
    EXPECT_EQ(static_cast<unsigned char>(contents[2]), 0xBFU);
    EXPECT_NE(contents.find("18446744073709551615"), std::string::npos);
    EXPECT_NE(contents.find("连续健康窗口"), std::string::npos);
    EXPECT_NE(contents.find("\"插件,\"\"值\"\"\""), std::string::npos);
    EXPECT_NE(contents.find("\"调试指标\""), std::string::npos);
    EXPECT_EQ(static_cast<std::size_t>(std::count(contents.begin(), contents.end(), '\n')), 37U);
    std::filesystem::remove(path, ignored);
}

TEST(AlgorithmMetrics, CsvRejectsStaleSnapshot)
{
    auto snapshot = valid_snapshot("CAM01", 1U);
    snapshot.stale = true;
    const auto path =
        std::filesystem::temp_directory_path() / "PaperBreakEdge-algorithm-metrics-stale.csv";
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    const auto exported = paperbreak::console::export_algorithm_current_values_csv(snapshot, path);
    ASSERT_FALSE(exported);
    EXPECT_EQ(exported.error().business_code, "ALGORITHM_SNAPSHOT_STALE");
    EXPECT_FALSE(std::filesystem::exists(path));
}

} // namespace
