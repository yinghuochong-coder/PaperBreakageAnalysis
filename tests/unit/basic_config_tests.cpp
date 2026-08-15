#include "paperbreak/config/basic_config.hpp"
#include "paperbreak/config/config_repository.hpp"
#include "paperbreak/platform/atomic_file.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace
{

class TemporaryDirectory final
{
  public:
    TemporaryDirectory()
    {
        static std::atomic_uint64_t sequence{0};
        path_ = std::filesystem::temp_directory_path() /
                ("paperbreak-config-test-" +
                 std::to_string(sequence.fetch_add(1U, std::memory_order_relaxed)));
        std::filesystem::create_directories(path_);
    }
    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }
    [[nodiscard]] std::filesystem::path write(std::string_view name,
                                              std::string_view contents) const
    {
        std::u8string converted;
        converted.reserve(name.size());
        for (const unsigned char byte : name)
            converted.push_back(static_cast<char8_t>(byte));
        const auto file = path_ / std::filesystem::path{converted};
        std::ofstream stream{file, std::ios::binary};
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        return file;
    }

  private:
    std::filesystem::path path_;
};

std::string valid_config()
{
    const auto path =
        std::filesystem::path{PAPERBREAK_TEST_SOURCE_DIR} / "data" / "basic-config-valid.json";
    std::ifstream stream{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

std::string version_seven_config()
{
    const auto parsed = paperbreak::config::parse_config(valid_config(), {});
    EXPECT_TRUE(parsed);
    if (!parsed)
        return {};
    auto document = nlohmann::json::parse(paperbreak::config::serialize_config(parsed.value()));
    document["configSchemaVersion"] = 7U;
    document.erase("timeSync");
    return document.dump(2) + "\n";
}

std::string replace_once(std::string value, const std::string_view from, const std::string_view to)
{
    const auto position = value.find(from);
    EXPECT_NE(position, std::string::npos);
    if (position != std::string::npos)
        value.replace(position, from.size(), to);
    return value;
}

std::string downgrade_algorithm_config(std::string value, const std::uint32_t schema_version,
                                       const std::uint32_t consecutive_frames = 3U)
{
    auto document = nlohmann::json::parse(value);
    document["configSchemaVersion"] = schema_version;
    auto& algorithm = document["algorithm"];
    algorithm.erase("downsampleMode");
    algorithm.erase("processingFps");
    algorithm.erase("confirmationDurationMs");
    algorithm.erase("rearmDurationMs");
    algorithm["consecutiveFrames"] = consecutive_frames;
    return document.dump(2) + "\n";
}

nlohmann::json camera_config(const std::string& camera_id, const std::string& serial_number)
{
    return {{"id", camera_id},
            {"enabled", true},
            {"serialNumber", serial_number},
            {"location", camera_id},
            {"exposureUs", 100.0},
            {"autoExposure", "Off"},
            {"gainDb", 2.0},
            {"frameRate", 60.0},
            {"roi", {{"width", 64U}, {"height", 48U}, {"offsetX", 0U}, {"offsetY", 0U}}},
            {"reverseX", false},
            {"reverseY", false},
            {"pixelFormat", "Mono8"},
            {"triggerMode", "Continuous"},
            {"triggerSource", ""},
            {"triggerDelayUs", 0U},
            {"packetSizeBytes", 1500U},
            {"interPacketDelayNs", 0U},
            {"lineIo",
             {{"alarmInputEnabled", false},
              {"alarmActiveLevel", "High"},
              {"strobeOutputEnabled", false},
              {"strobeDurationUs", 0U},
              {"strobePreDelayUs", 0U},
              {"strobePostDelayUs", 0U}}}};
}

class RecordingAudit final : public paperbreak::config::IConfigAuditSink
{
  public:
    paperbreak::Result<void> record(const paperbreak::config::ConfigAuditRecord& record) override
    {
        records.push_back(record);
        if (fail)
            return paperbreak::Result<void>::failure(paperbreak::make_error(
                "LOG_WRITE_FAILED", paperbreak::Severity::error, "injected", "test", "test.audit"));
        return paperbreak::Result<void>::success();
    }
    bool fail{};
    std::vector<paperbreak::config::ConfigAuditRecord> records;
};

class RecordingApplier final : public paperbreak::config::IConfigApplier
{
  public:
    [[nodiscard]] std::string_view name() const noexcept override
    {
        return "test-applier";
    }
    paperbreak::Result<void> prepare(const paperbreak::config::EdgeConfig&,
                                     const paperbreak::config::EdgeConfig&,
                                     const std::vector<std::string>&) override
    {
        calls.emplace_back("prepare");
        return paperbreak::Result<void>::success();
    }
    paperbreak::Result<void> apply_and_readback(const paperbreak::config::EdgeConfig&) override
    {
        calls.emplace_back("apply");
        if (fail_apply)
            return paperbreak::Result<void>::failure(
                paperbreak::make_error("CAMERA_CONFIG_FAILED", paperbreak::Severity::error,
                                       "injected", "test", "test.apply"));
        return paperbreak::Result<void>::success();
    }
    paperbreak::Result<void> commit(const paperbreak::config::EdgeConfig&) override
    {
        calls.emplace_back("commit");
        if (fail_commit)
            return paperbreak::Result<void>::failure(
                paperbreak::make_error("CAMERA_CONFIG_FAILED", paperbreak::Severity::error,
                                       "injected", "test", "test.commit"));
        return paperbreak::Result<void>::success();
    }
    paperbreak::Result<void> rollback(const paperbreak::config::EdgeConfig&) noexcept override
    {
        calls.emplace_back("rollback");
        return paperbreak::Result<void>::success();
    }
    bool fail_apply{};
    bool fail_commit{};
    std::vector<std::string> calls;
};

class FailingAtomicFileSystem final : public paperbreak::platform::IAtomicFileSystem
{
  public:
    paperbreak::Result<std::string> read_bounded(const std::filesystem::path& path,
                                                 const std::size_t maximum) override
    {
        return inner.read_bounded(path, maximum);
    }
    paperbreak::Result<void> create_directories(const std::filesystem::path& path) override
    {
        return inner.create_directories(path);
    }
    paperbreak::Result<std::vector<std::filesystem::path>> list_regular_files(
        const std::filesystem::path& path) override
    {
        return inner.list_regular_files(path);
    }
    paperbreak::Result<void> remove_file(const std::filesystem::path& path) override
    {
        return inner.remove_file(path);
    }
    paperbreak::Result<void> replace_atomically(
        const std::filesystem::path& path, const std::string_view contents,
        const std::optional<std::filesystem::path>& backup) override
    {
        ++replace_calls;
        if (fail_on_replace != 0U && replace_calls == fail_on_replace)
            return paperbreak::Result<void>::failure(
                paperbreak::make_error("SYS_CONFIG_PERSIST_FAILED", paperbreak::Severity::critical,
                                       "injected", "test", "test.replace"));
        return inner.replace_atomically(path, contents, backup);
    }
    paperbreak::platform::WindowsAtomicFileSystem inner;
    std::size_t replace_calls{};
    std::size_t fail_on_replace{};
};

} // namespace

TEST(BasicConfig, AcceptsCompleteVersionTwoAtUnicodeAndSpacePath)
{
    const TemporaryDirectory directory;
    const auto path = directory.write("纸机 配置.json", valid_config());
    const auto result = paperbreak::config::validate_basic_config(path);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result.value().schema_version, 8U);
    EXPECT_EQ(result.value().config_revision, 1U);
}

TEST(BasicConfig, DefaultsAndRoundTripsCameraStartupPolicy)
{
    const TemporaryDirectory directory;
    const auto legacy = paperbreak::config::parse_config(valid_config(), directory.path());
    ASSERT_TRUE(legacy) << legacy.error().message;
    EXPECT_FALSE(legacy.value().acquisition.auto_start);
    EXPECT_EQ(legacy.value().acquisition.startup_retry_interval_ms, 1000U);
    EXPECT_EQ(legacy.value().acquisition.startup_retry_count, 3U);

    auto explicit_policy = paperbreak::config::serialize_config(legacy.value());
    explicit_policy = replace_once(explicit_policy, "\"autoStart\": false", "\"autoStart\": true");
    explicit_policy = replace_once(explicit_policy, "\"startupRetryIntervalMs\": 1000",
                                   "\"startupRetryIntervalMs\": 250");
    explicit_policy =
        replace_once(explicit_policy, "\"startupRetryCount\": 3", "\"startupRetryCount\": 5");
    const auto parsed = paperbreak::config::parse_config(explicit_policy, directory.path());
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_TRUE(parsed.value().acquisition.auto_start);
    EXPECT_EQ(parsed.value().acquisition.startup_retry_interval_ms, 250U);
    EXPECT_EQ(parsed.value().acquisition.startup_retry_count, 5U);

    EXPECT_FALSE(paperbreak::config::parse_config(replace_once(explicit_policy,
                                                               "\"startupRetryIntervalMs\": 250",
                                                               "\"startupRetryIntervalMs\": 0"),
                                                  directory.path()));
    EXPECT_FALSE(paperbreak::config::parse_config(
        replace_once(explicit_policy, "\"startupRetryCount\": 5", "\"startupRetryCount\": 11"),
        directory.path()));
}

TEST(BasicConfig, MigratesVersionSevenTimeSyncDefaultsAndValidatesDependencies)
{
    const TemporaryDirectory directory;
    const auto migrated =
        paperbreak::config::parse_config(version_seven_config(), directory.path());
    ASSERT_TRUE(migrated) << migrated.error().message;
    EXPECT_EQ(migrated.value().config_schema_version, 8U);
    EXPECT_EQ(migrated.value().time_sync.sample_period_ms, 1000U);
    EXPECT_EQ(migrated.value().time_sync.probe_timeout_ms, 250U);
    EXPECT_EQ(migrated.value().time_sync.receive_clock_uncertainty_ns, 50'000'000);
    EXPECT_EQ(migrated.value().time_sync.warning_threshold_ns, 1'000'000);
    EXPECT_EQ(migrated.value().time_sync.alarm_threshold_ns, 5'000'000);
    EXPECT_EQ(migrated.value().time_sync.warning_duration_ms, 3000U);
    EXPECT_EQ(migrated.value().time_sync.alarm_duration_ms, 3000U);

    auto document = nlohmann::json::parse(paperbreak::config::serialize_config(migrated.value()));
    document["timeSync"]["probeTimeoutMs"] = 1000U;
    EXPECT_FALSE(paperbreak::config::parse_config(document.dump(), directory.path()));
    document["timeSync"]["probeTimeoutMs"] = 250U;
    document["timeSync"]["warningThresholdNs"] = 5'000'000U;
    EXPECT_FALSE(paperbreak::config::parse_config(document.dump(), directory.path()));
    document["timeSync"]["warningThresholdNs"] = 1'000'000U;
    document["timeSync"]["extra"] = true;
    EXPECT_FALSE(paperbreak::config::parse_config(document.dump(), directory.path()));
}

TEST(BasicConfig, AllowsAlgorithmFullFrameAcrossDifferentCameraGeometries)
{
    const TemporaryDirectory directory;
    auto contents = replace_once(
        valid_config(), "\"cameras\": []",
        R"("cameras": [{"id":"CAM01","enabled":true,"serialNumber":"MOCK-01","location":"入口","exposureUs":100.0,"gainDb":2.0,"frameRate":60.0,"roi":{"width":1624,"height":1240,"offsetX":0,"offsetY":0},"pixelFormat":"Mono8","triggerMode":"Continuous","triggerSource":"","triggerDelayUs":0,"packetSizeBytes":1500,"interPacketDelayNs":0},{"id":"CAM02","enabled":true,"serialNumber":"MOCK-02","location":"出口","exposureUs":100.0,"gainDb":2.0,"frameRate":60.0,"roi":{"width":800,"height":600,"offsetX":0,"offsetY":0},"pixelFormat":"Mono8","triggerMode":"Continuous","triggerSource":"","triggerDelayUs":0,"packetSizeBytes":1500,"interPacketDelayNs":0}])");
    contents =
        replace_once(contents, R"("roi": { "width": 1, "height": 1, "offsetX": 0, "offsetY": 0 })",
                     R"("roi": { "width": 0, "height": 0, "offsetX": 0, "offsetY": 0 })");

    const auto parsed = paperbreak::config::parse_config(contents, directory.path());
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_EQ(parsed.value().algorithm.roi.width, 0U);
    EXPECT_EQ(parsed.value().algorithm.roi.height, 0U);
    const auto serialized = paperbreak::config::serialize_config(parsed.value());
    EXPECT_NE(serialized.find("\"width\": 0"), std::string::npos);
}

TEST(BasicConfig, RejectsMalformedOrOutOfCameraAlgorithmRoiAndKeepsCameraRoiNonzero)
{
    const TemporaryDirectory directory;
    const auto with_camera = replace_once(
        valid_config(), "\"cameras\": []",
        R"("cameras": [{"id":"CAM01","enabled":true,"serialNumber":"MOCK-01","location":"入口","exposureUs":100.0,"gainDb":2.0,"frameRate":60.0,"roi":{"width":800,"height":600,"offsetX":0,"offsetY":0},"pixelFormat":"Mono8","triggerMode":"Continuous","triggerSource":"","triggerDelayUs":0,"packetSizeBytes":1500,"interPacketDelayNs":0}])");
    const auto algorithm_roi = R"("roi": { "width": 1, "height": 1, "offsetX": 0, "offsetY": 0 })";

    const std::vector<std::string> invalid_algorithm_rois{
        R"("roi": { "width": 0, "height": 1, "offsetX": 0, "offsetY": 0 })",
        R"("roi": { "width": 0, "height": 0, "offsetX": 1, "offsetY": 0 })",
        R"("roi": { "width": 801, "height": 600, "offsetX": 0, "offsetY": 0 })",
        R"("roi": { "width": 800, "height": 600, "offsetX": 1, "offsetY": 0 })",
    };
    for (const auto& roi : invalid_algorithm_rois)
    {
        const auto parsed = paperbreak::config::parse_config(
            replace_once(with_camera, algorithm_roi, roi), directory.path());
        EXPECT_FALSE(parsed);
        if (!parsed)
            EXPECT_EQ(parsed.error().business_code, "SYS_CONFIG_INVALID");
    }

    const auto zero_camera =
        replace_once(with_camera, R"("roi":{"width":800,"height":600,"offsetX":0,"offsetY":0})",
                     R"("roi":{"width":0,"height":0,"offsetX":0,"offsetY":0})");
    const auto parsed_camera = paperbreak::config::parse_config(zero_camera, directory.path());
    EXPECT_FALSE(parsed_camera);
    if (!parsed_camera)
        EXPECT_EQ(parsed_camera.error().business_code, "SYS_CONFIG_INVALID");
}

TEST(BasicConfig, DefaultsLegacyCameraMirroringOffAndSerializesExplicitValues)
{
    const TemporaryDirectory directory;
    auto contents = replace_once(
        valid_config(), "\"cameras\": []",
        R"("cameras": [{"id":"CAM01","enabled":true,"serialNumber":"MOCK-01","location":"入口","exposureUs":100.0,"gainDb":2.0,"frameRate":30.0,"roi":{"width":64,"height":48,"offsetX":0,"offsetY":0},"pixelFormat":"Mono8","triggerMode":"Continuous","triggerSource":"","triggerDelayUs":0,"packetSizeBytes":1500,"interPacketDelayNs":0}])");
    auto parsed = paperbreak::config::parse_config(contents, directory.path());
    ASSERT_TRUE(parsed) << parsed.error().message;
    ASSERT_EQ(parsed.value().cameras.size(), 1U);
    EXPECT_FALSE(parsed.value().cameras.front().reverse_x);
    EXPECT_FALSE(parsed.value().cameras.front().reverse_y);

    contents = replace_once(contents, "\"pixelFormat\":\"Mono8\"",
                            "\"reverseX\":true,\"reverseY\":true,\"pixelFormat\":\"Mono8\"");
    parsed = paperbreak::config::parse_config(contents, directory.path());
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_TRUE(parsed.value().cameras.front().reverse_x);
    EXPECT_TRUE(parsed.value().cameras.front().reverse_y);
    const auto serialized = paperbreak::config::serialize_config(parsed.value());
    EXPECT_NE(serialized.find("\"reverseX\": true"), std::string::npos);
    EXPECT_NE(serialized.find("\"reverseY\": true"), std::string::npos);
}

TEST(BasicConfig, MigratesVersionTwoCameraDefaultsToVersionEight)
{
    const TemporaryDirectory directory;
    const auto contents = replace_once(
        valid_config(), "\"cameras\": []",
        R"("cameras": [{"id":"CAM01","enabled":true,"serialNumber":"MOCK-01","location":"入口","exposureUs":321.0,"gainDb":2.0,"frameRate":30.0,"roi":{"width":64,"height":48,"offsetX":0,"offsetY":0},"pixelFormat":"Mono8","triggerMode":"Continuous","triggerSource":"","triggerDelayUs":0,"packetSizeBytes":1500,"interPacketDelayNs":0}])");
    const auto parsed = paperbreak::config::parse_config(contents, directory.path());
    ASSERT_TRUE(parsed) << parsed.error().message;
    ASSERT_EQ(parsed.value().cameras.size(), 1U);
    const auto& camera = parsed.value().cameras.front();
    EXPECT_EQ(parsed.value().config_schema_version, 8U);
    EXPECT_EQ(parsed.value().algorithm.downsample_mode,
              paperbreak::config::AlgorithmDownsampleMode::disabled);
    EXPECT_EQ(parsed.value().algorithm.processing_fps,
              paperbreak::config::AlgorithmProcessingFps::fps60);
    EXPECT_EQ(parsed.value().algorithm.confirmation_duration_ms, 50U);
    EXPECT_EQ(parsed.value().algorithm.rearm_duration_ms, 500U);
    EXPECT_DOUBLE_EQ(camera.exposure_us, 321.0);
    EXPECT_EQ(camera.exposure_auto_mode, paperbreak::config::ExposureAutoMode::off);
    EXPECT_FALSE(camera.line_io.alarm_input_enabled);
    EXPECT_EQ(camera.line_io.alarm_active_level, paperbreak::config::AlarmActiveLevel::high);
    EXPECT_FALSE(camera.line_io.strobe_output_enabled);
    EXPECT_EQ(camera.line_io.strobe_duration_us, 0U);
    EXPECT_EQ(camera.line_io.strobe_pre_delay_us, 0U);
    EXPECT_EQ(camera.line_io.strobe_post_delay_us, 0U);

    const auto serialized = paperbreak::config::serialize_config(parsed.value());
    EXPECT_NE(serialized.find("\"configSchemaVersion\": 8"), std::string::npos);
    EXPECT_NE(serialized.find("\"autoExposure\": \"Off\""), std::string::npos);
    EXPECT_NE(serialized.find("\"lineIo\""), std::string::npos);
}

TEST(BasicConfig, MigratesVersionThreeAutoExposureOffAndRoundTripsVersionSixModes)
{
    const TemporaryDirectory directory;
    const auto version_two = replace_once(
        valid_config(), "\"cameras\": []",
        R"("cameras": [{"id":"CAM01","enabled":true,"serialNumber":"MOCK-01","location":"入口","exposureUs":321.0,"gainDb":2.0,"frameRate":30.0,"roi":{"width":64,"height":48,"offsetX":0,"offsetY":0},"pixelFormat":"Mono8","triggerMode":"Continuous","triggerSource":"","triggerDelayUs":0,"packetSizeBytes":1500,"interPacketDelayNs":0}])");
    const auto migrated_v2 = paperbreak::config::parse_config(version_two, directory.path());
    ASSERT_TRUE(migrated_v2);
    const auto version_six = paperbreak::config::serialize_config(migrated_v2.value());
    auto version_three = downgrade_algorithm_config(version_six, 3U);
    version_three = replace_once(version_three, "\"autoExposure\": \"Off\",\n", "");

    const auto migrated_v3 = paperbreak::config::parse_config(version_three, directory.path());
    ASSERT_TRUE(migrated_v3) << migrated_v3.error().message;
    EXPECT_EQ(migrated_v3.value().cameras.front().exposure_auto_mode,
              paperbreak::config::ExposureAutoMode::off);

    const auto once =
        replace_once(version_six, "\"autoExposure\": \"Off\"", "\"autoExposure\": \"Once\"");
    const auto parsed_once = paperbreak::config::parse_config(once, directory.path());
    ASSERT_TRUE(parsed_once) << parsed_once.error().message;
    EXPECT_EQ(parsed_once.value().cameras.front().exposure_auto_mode,
              paperbreak::config::ExposureAutoMode::once);
    EXPECT_NE(paperbreak::config::serialize_config(parsed_once.value())
                  .find("\"autoExposure\": \"Once\""),
              std::string::npos);

    const auto unknown =
        replace_once(version_six, "\"autoExposure\": \"Off\"", "\"autoExposure\": \"Adaptive\"");
    EXPECT_FALSE(paperbreak::config::parse_config(unknown, directory.path()));
}

TEST(BasicConfig, MigratesVersionFourSevenFramesToOneHundredTwentyMilliseconds)
{
    const TemporaryDirectory directory;
    auto legacy = paperbreak::config::parse_config(valid_config(), directory.path());
    ASSERT_TRUE(legacy);
    const auto version_four =
        downgrade_algorithm_config(paperbreak::config::serialize_config(legacy.value()), 4U, 7U);
    auto migrated = paperbreak::config::parse_config(version_four, directory.path());
    ASSERT_TRUE(migrated) << migrated.error().message;
    EXPECT_EQ(migrated.value().algorithm.downsample_mode,
              paperbreak::config::AlgorithmDownsampleMode::disabled);
    EXPECT_EQ(migrated.value().algorithm.processing_fps,
              paperbreak::config::AlgorithmProcessingFps::fps60);
    EXPECT_EQ(migrated.value().algorithm.confirmation_duration_ms, 120U);
    EXPECT_EQ(migrated.value().algorithm.rearm_duration_ms, 500U);
    const auto serialized =
        nlohmann::json::parse(paperbreak::config::serialize_config(migrated.value()));
    EXPECT_EQ(serialized["configSchemaVersion"], 8U);
    EXPECT_FALSE(serialized["algorithm"].contains("consecutiveFrames"));
}

TEST(BasicConfig, MigratesVersionFiveRearmDefaultAndStrictlyValidatesVersionEight)
{
    const TemporaryDirectory directory;
    auto migrated = paperbreak::config::parse_config(valid_config(), directory.path());
    ASSERT_TRUE(migrated);
    auto document = nlohmann::json::parse(paperbreak::config::serialize_config(migrated.value()));

    auto version_five = document;
    version_five["configSchemaVersion"] = 5U;
    version_five["algorithm"].erase("rearmDurationMs");
    auto migrated_v5 = paperbreak::config::parse_config(version_five.dump(), directory.path());
    ASSERT_TRUE(migrated_v5) << migrated_v5.error().message;
    EXPECT_EQ(migrated_v5.value().algorithm.rearm_duration_ms, 500U);
    EXPECT_EQ(nlohmann::json::parse(
                  paperbreak::config::serialize_config(migrated_v5.value()))["configSchemaVersion"],
              8U);

    for (const std::string_view mode : {"disabled", "half", "quarter"})
        for (const auto fps : {15U, 30U, 60U})
        {
            document["algorithm"]["downsampleMode"] = mode;
            document["algorithm"]["processingFps"] = fps;
            document["algorithm"]["confirmationDurationMs"] = 120U;
            document["algorithm"]["rearmDurationMs"] = 500U;
            auto parsed = paperbreak::config::parse_config(document.dump(), directory.path());
            ASSERT_TRUE(parsed) << parsed.error().message;
            EXPECT_EQ(static_cast<std::uint32_t>(parsed.value().algorithm.processing_fps), fps);
        }

    auto invalid_mode = document;
    invalid_mode["algorithm"]["downsampleMode"] = "eighth";
    EXPECT_FALSE(paperbreak::config::parse_config(invalid_mode.dump(), directory.path()));
    auto invalid_fps = document;
    invalid_fps["algorithm"]["processingFps"] = 20U;
    EXPECT_FALSE(paperbreak::config::parse_config(invalid_fps.dump(), directory.path()));
    auto too_short = document;
    too_short["algorithm"]["confirmationDurationMs"] = 9U;
    EXPECT_FALSE(paperbreak::config::parse_config(too_short.dump(), directory.path()));
    auto exceeds_timeout = document;
    exceeds_timeout["algorithm"]["confirmationDurationMs"] = 60000U;
    exceeds_timeout["event"]["maxEventSeconds"] = 10U;
    exceeds_timeout["event"]["preEventSeconds"] = 5U;
    exceeds_timeout["event"]["postEventSeconds"] = 5U;
    EXPECT_FALSE(paperbreak::config::parse_config(exceeds_timeout.dump(), directory.path()));
    auto unknown = document;
    unknown["algorithm"]["extra"] = true;
    EXPECT_FALSE(paperbreak::config::parse_config(unknown.dump(), directory.path()));
    for (const auto boundary : {0U, 3600000U})
    {
        auto valid_rearm = document;
        valid_rearm["algorithm"]["rearmDurationMs"] = boundary;
        auto parsed = paperbreak::config::parse_config(valid_rearm.dump(), directory.path());
        ASSERT_TRUE(parsed) << parsed.error().message;
        EXPECT_EQ(parsed.value().algorithm.rearm_duration_ms, boundary);
    }
    auto excessive_rearm = document;
    excessive_rearm["algorithm"]["rearmDurationMs"] = 3600001U;
    EXPECT_FALSE(paperbreak::config::parse_config(excessive_rearm.dump(), directory.path()));
    auto missing_rearm = document;
    missing_rearm["algorithm"].erase("rearmDurationMs");
    EXPECT_FALSE(paperbreak::config::parse_config(missing_rearm.dump(), directory.path()));
}

TEST(BasicConfig, StrictlyValidatesVersionThreeLineIoAndDependencies)
{
    const TemporaryDirectory directory;
    const auto legacy = replace_once(
        valid_config(), "\"cameras\": []",
        R"("cameras": [{"id":"CAM01","enabled":true,"serialNumber":"MOCK-01","location":"入口","exposureUs":100.0,"gainDb":2.0,"frameRate":30.0,"roi":{"width":64,"height":48,"offsetX":0,"offsetY":0},"pixelFormat":"Mono8","triggerMode":"Continuous","triggerSource":"","triggerDelayUs":0,"packetSizeBytes":1500,"interPacketDelayNs":0}])");
    const auto migrated = paperbreak::config::parse_config(legacy, directory.path());
    ASSERT_TRUE(migrated);
    const auto version_three = paperbreak::config::serialize_config(migrated.value());

    const auto low = replace_once(version_three, "\"alarmActiveLevel\": \"High\"",
                                  "\"alarmActiveLevel\": \"Low\"");
    const auto low_result = paperbreak::config::parse_config(low, directory.path());
    ASSERT_TRUE(low_result) << low_result.error().message;
    EXPECT_EQ(low_result.value().cameras.front().line_io.alarm_active_level,
              paperbreak::config::AlarmActiveLevel::low);

    const auto missing = replace_once(version_three, "\"alarmInputEnabled\": false,\n", "");
    EXPECT_FALSE(paperbreak::config::parse_config(missing, directory.path()));
    const auto unknown = replace_once(version_three, "\"alarmInputEnabled\": false,",
                                      "\"alarmInputEnabled\": false, \"extra\": 1,");
    EXPECT_FALSE(paperbreak::config::parse_config(unknown, directory.path()));
    const auto disabled_camera =
        replace_once(replace_once(version_three, "\"enabled\": true", "\"enabled\": false"),
                     "\"alarmInputEnabled\": false", "\"alarmInputEnabled\": true");
    EXPECT_FALSE(paperbreak::config::parse_config(disabled_camera, directory.path()));
    const auto zero_duration = replace_once(version_three, "\"strobeOutputEnabled\": false",
                                            "\"strobeOutputEnabled\": true");
    EXPECT_FALSE(paperbreak::config::parse_config(zero_duration, directory.path()));
}

TEST(BasicConfig, AcceptsSixVersionEightCamerasAndRejectsInvalidSeventhOrDuplicates)
{
    const TemporaryDirectory directory;
    auto baseline = paperbreak::config::parse_config(valid_config(), directory.path());
    ASSERT_TRUE(baseline) << baseline.error().message;
    auto document = nlohmann::json::parse(paperbreak::config::serialize_config(baseline.value()));
    document["cameras"] = nlohmann::json::array();
    for (std::size_t index = 0U; index < paperbreak::camera_slot_count; ++index)
    {
        document["cameras"].push_back(
            camera_config(std::string{paperbreak::canonical_camera_ids[index]},
                          "MOCK-" + std::to_string(index + 1U)));
    }

    const auto parsed = paperbreak::config::parse_config(document.dump(), directory.path());
    ASSERT_TRUE(parsed) << parsed.error().message;
    ASSERT_EQ(parsed.value().cameras.size(), paperbreak::camera_slot_count);
    EXPECT_EQ(parsed.value().cameras.back().id, "CAM06");
    const auto round_trip = paperbreak::config::parse_config(
        paperbreak::config::serialize_config(parsed.value()), directory.path());
    ASSERT_TRUE(round_trip) << round_trip.error().message;
    EXPECT_EQ(round_trip.value(), parsed.value());

    auto version_six = document;
    version_six["configSchemaVersion"] = 6U;
    const auto migrated_v6 = paperbreak::config::parse_config(version_six.dump(), directory.path());
    ASSERT_TRUE(migrated_v6) << migrated_v6.error().message;
    EXPECT_EQ(migrated_v6.value().config_schema_version, 8U);

    auto seventh = document;
    seventh["cameras"].push_back(camera_config("CAM07", "MOCK-7"));
    EXPECT_FALSE(paperbreak::config::parse_config(seventh.dump(), directory.path()));

    auto invalid_id = document;
    invalid_id["cameras"][5]["id"] = "CAM07";
    EXPECT_FALSE(paperbreak::config::parse_config(invalid_id.dump(), directory.path()));

    auto duplicate_id = document;
    duplicate_id["cameras"][5]["id"] = "CAM01";
    EXPECT_FALSE(paperbreak::config::parse_config(duplicate_id.dump(), directory.path()));

    auto duplicate_serial = document;
    duplicate_serial["cameras"][5]["serialNumber"] = "MOCK-1";
    EXPECT_FALSE(paperbreak::config::parse_config(duplicate_serial.dump(), directory.path()));

    auto future = document;
    future["configSchemaVersion"] = 9U;
    const auto future_result = paperbreak::config::parse_config(future.dump(), directory.path());
    ASSERT_FALSE(future_result);
    EXPECT_EQ(future_result.error().business_code, "SYS_CONFIG_SCHEMA_UNSUPPORTED");
}

TEST(BasicConfig, RejectsUnknownSensitiveMalformedAndUnsupportedSchema)
{
    const TemporaryDirectory directory;
    const auto unknown =
        directory.write("unknown.json", replace_once(valid_config(), "\"health\": {",
                                                     "\"extra\": 1, \"health\": {"));
    const auto sensitive =
        directory.write("secret.json", replace_once(valid_config(), "\"serverUrl\": \"\"",
                                                    "\"serverUrl\": \"\", \"token\": \"raw\""));
    const auto malformed = directory.write("truncated.json", R"({"configSchemaVersion":1)");
    const auto future =
        directory.write("future.json", replace_once(valid_config(), "\"configSchemaVersion\": 2",
                                                    "\"configSchemaVersion\": 9"));
    EXPECT_FALSE(paperbreak::config::validate_basic_config(unknown));
    EXPECT_FALSE(paperbreak::config::validate_basic_config(sensitive));
    EXPECT_FALSE(paperbreak::config::validate_basic_config(malformed));
    const auto future_result = paperbreak::config::validate_basic_config(future);
    ASSERT_FALSE(future_result);
    EXPECT_EQ(future_result.error().business_code, "SYS_CONFIG_SCHEMA_UNSUPPORTED");
}

TEST(BasicConfig, RejectsCrossFieldAndPathViolations)
{
    const TemporaryDirectory directory;
    const auto event =
        directory.write("event.json", replace_once(valid_config(), "\"maxEventSeconds\": 60",
                                                   "\"maxEventSeconds\": 15"));
    const auto watermarks = directory.write(
        "watermarks.json", replace_once(valid_config(), "\"criticalFreeSpaceGiB\": 100",
                                        "\"criticalFreeSpaceGiB\": 250"));
    const auto path = directory.write("path.json", replace_once(valid_config(),
                                                                "\"eventRoot\": \"数据/事件 文件\"",
                                                                "\"eventRoot\": \"../escape\""));
    EXPECT_FALSE(paperbreak::config::validate_basic_config(event));
    EXPECT_FALSE(paperbreak::config::validate_basic_config(watermarks));
    EXPECT_FALSE(paperbreak::config::validate_basic_config(path));
}

TEST(BasicConfig, AcceptsOnlyBoundedPlaintextUplinkV1Configuration)
{
    const TemporaryDirectory directory;
    auto enabled = replace_once(valid_config(), "\"enabled\": false, \"serverUrl\": \"\"",
                                "\"enabled\": true, \"serverUrl\": \"http://127.0.0.1:18080\"");
    auto parsed = paperbreak::config::parse_config(enabled, directory.path());
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_EQ(parsed.value().uplink.chunk_bytes, 1024U * 1024U);
    EXPECT_EQ(parsed.value().uplink.io_timeout_ms, 10000U);
    EXPECT_EQ(parsed.value().uplink.upload_limit_mibps, 20U);
    const auto serialized = paperbreak::config::serialize_config(parsed.value());
    EXPECT_NE(serialized.find("\"uploadLimitMiBps\": 20"), std::string::npos);

    const auto https = replace_once(enabled, "http://127.0.0.1:18080", "https://127.0.0.1:18080");
    EXPECT_FALSE(paperbreak::config::parse_config(https, directory.path()));
    const auto credential = replace_once(enabled, "\"credentialReference\": \"\"",
                                         "\"credentialReference\": \"legacy-secret\"");
    EXPECT_FALSE(paperbreak::config::parse_config(credential, directory.path()));
    const auto oversized_chunk =
        replace_once(enabled, "\"chunkBytes\": 1048576", "\"chunkBytes\": 4194305");
    EXPECT_FALSE(paperbreak::config::parse_config(oversized_chunk, directory.path()));
}

TEST(BasicConfig, ValidatesAndSerializesVersionTwoNvmeSettings)
{
    const TemporaryDirectory directory;
    auto parsed = paperbreak::config::parse_config(valid_config(), directory.path());
    ASSERT_TRUE(parsed);
    EXPECT_FALSE(parsed.value().storage.rolling_cache_enabled);
    EXPECT_EQ(parsed.value().storage.maximum_cache_storage_gib, 1000U);
    EXPECT_EQ(parsed.value().storage.rolling_cache_write_limit_mibps, 600U);
    EXPECT_EQ(parsed.value().storage.rolling_cache_io_timeout_ms, 10000U);
    EXPECT_NE(paperbreak::config::serialize_config(parsed.value())
                  .find("\"rollingCacheIoTimeoutMs\": 10000"),
              std::string::npos);

    const auto invalid = directory.write(
        "invalid-timeout.json", replace_once(valid_config(), "\"rollingCacheIoTimeoutMs\": 10000",
                                             "\"rollingCacheIoTimeoutMs\": 99"));
    EXPECT_FALSE(paperbreak::config::validate_basic_config(invalid));

    auto changed = parsed.value();
    changed.storage.rolling_cache_enabled = true;
    const auto paths = paperbreak::config::changed_config_paths(parsed.value(), changed);
    EXPECT_NE(std::ranges::find(paths, "/storage/nvme"), paths.end());
    EXPECT_TRUE(paperbreak::config::is_restart_required_path("/storage/nvme"));
}

TEST(BasicConfig, RejectsEmptyMissingDirectoryAndOversizedFiles)
{
    const TemporaryDirectory directory;
    EXPECT_FALSE(paperbreak::config::validate_basic_config(directory.write("empty.json", "")));
    EXPECT_FALSE(paperbreak::config::validate_basic_config(directory.path() / "missing.json"));
    EXPECT_FALSE(paperbreak::config::validate_basic_config(directory.path()));
    const std::string oversized(paperbreak::config::config_max_bytes + 1U, 'x');
    EXPECT_FALSE(
        paperbreak::config::validate_basic_config(directory.write("large.json", oversized)));
}

TEST(ConfigRepository, CommitsHotChangesAndIsIdempotent)
{
    const TemporaryDirectory directory;
    const auto path = directory.write("config.json", valid_config());
    paperbreak::platform::WindowsAtomicFileSystem files;
    RecordingAudit audit;
    RecordingApplier applier;
    paperbreak::config::ConfigRepository repository{path, files, audit, {&applier}};
    ASSERT_TRUE(repository.load());
    const auto candidate = replace_once(valid_config(), "\"fps\": 3.0", "\"fps\": 4.0");
    const auto updated =
        repository.update(candidate, 1U,
                          {.source = paperbreak::config::ConfigChangeSource::local_file,
                           .actor = "operator",
                           .correlation_id = "req-1"});
    ASSERT_TRUE(updated) << updated.error().message;
    EXPECT_EQ(updated.value().stored_config_revision, 2U);
    EXPECT_EQ(updated.value().effective_config_revision, 2U);
    EXPECT_TRUE(updated.value().pending_restart_paths.empty());
    EXPECT_EQ(audit.records.size(), 1U);
    EXPECT_EQ(applier.calls, (std::vector<std::string>{"prepare", "apply", "commit"}));

    auto current_json = paperbreak::config::serialize_config(*updated.value().stored);
    const auto idempotent =
        repository.update(current_json, 2U,
                          {.source = paperbreak::config::ConfigChangeSource::local_file,
                           .actor = "operator",
                           .correlation_id = "req-2"});
    ASSERT_TRUE(idempotent);
    EXPECT_EQ(idempotent.value().stored_config_revision, 2U);
    EXPECT_EQ(audit.records.size(), 1U);
}

TEST(ConfigRepository, ReportsVersionConflictAndPendingRestartWithoutApplyingStoredIdentity)
{
    const TemporaryDirectory directory;
    const auto path = directory.write("config.json", valid_config());
    paperbreak::platform::WindowsAtomicFileSystem files;
    RecordingAudit audit;
    paperbreak::config::ConfigRepository repository{path, files, audit};
    ASSERT_TRUE(repository.load());
    const auto candidate = replace_once(valid_config(), "EDGE-TEST", "EDGE-NEW");
    const auto conflict = repository.update(candidate, 9U, {});
    ASSERT_FALSE(conflict);
    EXPECT_EQ(conflict.error().business_code, "SYS_CONFIG_VERSION_CONFLICT");
    const auto updated = repository.update(candidate, 1U, {});
    ASSERT_TRUE(updated);
    EXPECT_EQ(updated.value().stored->system.machine_id, "EDGE-NEW");
    EXPECT_EQ(updated.value().effective->system.machine_id, "EDGE-TEST");
    EXPECT_EQ(updated.value().effective_config_revision, 1U);
    EXPECT_NE(std::ranges::find(updated.value().pending_restart_paths, "/system"),
              updated.value().pending_restart_paths.end());
}

TEST(ConfigRepository, RegistersDynamicApplierIdempotentlyAndRejectsAfterStop)
{
    const TemporaryDirectory directory;
    const auto path = directory.write("config.json", valid_config());
    paperbreak::platform::WindowsAtomicFileSystem files;
    RecordingAudit audit;
    RecordingApplier applier;
    paperbreak::config::ConfigRepository repository{path, files, audit};
    ASSERT_TRUE(repository.load());
    ASSERT_TRUE(repository.register_applier(applier));
    ASSERT_TRUE(repository.register_applier(applier));

    const auto candidate = replace_once(valid_config(), "\"fps\": 3.0", "\"fps\": 4.0");
    ASSERT_TRUE(repository.update(candidate, 1U, {}));
    EXPECT_EQ(applier.calls, (std::vector<std::string>{"prepare", "apply", "commit"}));

    RecordingApplier late_applier;
    repository.stop_accepting_changes();
    const auto rejected = repository.register_applier(late_applier);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().business_code, "SYS_SERVICE_STOPPING");
}

TEST(ConfigRepository, RollsBackAppliedComponentsOnApplyAndPersistenceFailures)
{
    const TemporaryDirectory directory;
    const auto path = directory.write("config.json", valid_config());
    FailingAtomicFileSystem files;
    RecordingAudit audit;
    RecordingApplier applier;
    paperbreak::config::ConfigRepository repository{path, files, audit, {&applier}};
    ASSERT_TRUE(repository.load());
    const auto candidate = replace_once(valid_config(), "\"fps\": 3.0", "\"fps\": 4.0");

    applier.fail_apply = true;
    const auto apply_failure = repository.update(candidate, 1U, {});
    ASSERT_FALSE(apply_failure);
    EXPECT_EQ(apply_failure.error().business_code, "SYS_CONFIG_APPLY_FAILED");
    EXPECT_EQ(applier.calls, (std::vector<std::string>{"prepare", "apply", "rollback"}));
    applier.fail_apply = false;
    applier.calls.clear();
    files.fail_on_replace = 2U;
    const auto write_failure = repository.update(candidate, 1U, {});
    ASSERT_FALSE(write_failure);
    EXPECT_EQ(write_failure.error().business_code, "SYS_CONFIG_PERSIST_FAILED");
    EXPECT_EQ(applier.calls, (std::vector<std::string>{"prepare", "apply", "rollback"}));
    ASSERT_TRUE(repository.snapshot());
    EXPECT_EQ(repository.snapshot().value().stored_config_revision, 1U);
}

TEST(ConfigRepository, RestoresDiskAndRuntimeWhenCommitFails)
{
    const TemporaryDirectory directory;
    const auto path = directory.write("config.json", valid_config());
    paperbreak::platform::WindowsAtomicFileSystem files;
    RecordingAudit audit;
    RecordingApplier applier;
    applier.fail_commit = true;
    paperbreak::config::ConfigRepository repository{path, files, audit, {&applier}};
    ASSERT_TRUE(repository.load());
    const auto result =
        repository.update(replace_once(valid_config(), "\"fps\": 3.0", "\"fps\": 4.0"), 1U, {});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().business_code, "SYS_CONFIG_APPLY_FAILED");
    EXPECT_EQ(applier.calls, (std::vector<std::string>{"prepare", "apply", "commit", "rollback"}));
    ASSERT_TRUE(repository.snapshot());
    EXPECT_EQ(repository.snapshot().value().stored_config_revision, 1U);
    const auto persisted = files.read_bounded(path, paperbreak::config::config_max_bytes);
    ASSERT_TRUE(persisted);
    EXPECT_EQ(paperbreak::config::parse_config(persisted.value(), directory.path())
                  .value()
                  .config_revision,
              1U);
}

TEST(ConfigRepository, AuditValuesAreBoundedAndReferencesAreRedacted)
{
    const TemporaryDirectory directory;
    const auto path = directory.write("config.json", valid_config());
    paperbreak::platform::WindowsAtomicFileSystem files;
    RecordingAudit audit;
    paperbreak::config::ConfigRepository repository{path, files, audit};
    ASSERT_TRUE(repository.load());
    const auto candidate = replace_once(valid_config(), "\"credentialReference\": \"\"",
                                        "\"credentialReference\": \"vault-entry-42\"");
    ASSERT_TRUE(repository.update(candidate, 1U, {}));
    ASSERT_EQ(audit.records.size(), 1U);
    ASSERT_FALSE(audit.records.front().redacted_changes.empty());
    for (const auto& change : audit.records.front().redacted_changes)
    {
        EXPECT_EQ(change.previous_value.find("vault-entry-42"), std::string::npos);
        EXPECT_EQ(change.candidate_value.find("vault-entry-42"), std::string::npos);
        EXPECT_LE(change.previous_value.size(), 4110U);
        EXPECT_LE(change.candidate_value.size(), 4110U);
    }
}

TEST(ConfigRepository, RecoversNewestValidHistoryAndIgnoresTemporaryResidue)
{
    const TemporaryDirectory directory;
    const auto path = directory.write("config.json", "{truncated");
    const auto history = directory.path() / "config.json.history";
    std::filesystem::create_directories(history);
    const auto valid =
        replace_once(valid_config(), "\"configRevision\": 1", "\"configRevision\": 7");
    std::ofstream{history / "00000000000000000007.json", std::ios::binary} << valid;
    std::ofstream{directory.path() / "config.json.paperbreak.tmp.1.1", std::ios::binary}
        << replace_once(valid, "\"configRevision\": 7", "\"configRevision\": 99");
    paperbreak::platform::WindowsAtomicFileSystem files;
    RecordingAudit audit;
    paperbreak::config::ConfigRepository repository{path, files, audit};
    const auto loaded = repository.load();
    ASSERT_TRUE(loaded) << loaded.error().message;
    EXPECT_TRUE(loaded.value().recovered_from_history);
    EXPECT_EQ(loaded.value().stored_config_revision, 7U);
    EXPECT_EQ(audit.records.front().source,
              paperbreak::config::ConfigChangeSource::startup_recovery);
}

TEST(ConfigRepository, AtomicallyMigratesVersionSevenAndKeepsLegacyHistory)
{
    const TemporaryDirectory directory;
    const auto path = directory.write("config.json", version_seven_config());
    paperbreak::platform::WindowsAtomicFileSystem files;
    RecordingAudit audit;
    paperbreak::config::ConfigRepository repository{path, files, audit};
    const auto loaded = repository.load();
    ASSERT_TRUE(loaded) << loaded.error().message;
    EXPECT_EQ(loaded.value().stored->config_schema_version, 8U);
    ASSERT_EQ(audit.records.size(), 1U);
    EXPECT_EQ(audit.records.front().correlation_id, "schema-v7-to-v8");
    std::ifstream migrated_stream{path, std::ios::binary};
    const std::string migrated{std::istreambuf_iterator<char>{migrated_stream},
                               std::istreambuf_iterator<char>{}};
    EXPECT_EQ(nlohmann::json::parse(migrated)["configSchemaVersion"], 8U);
    EXPECT_TRUE(std::filesystem::is_regular_file(directory.path() / "config.json.history" /
                                                 "00000000000000000001.v7.json"));
}

TEST(ConfigRepository, VersionSevenMigrationFailureLeavesMainConfigUntouched)
{
    const TemporaryDirectory directory;
    const auto path = directory.write("config.json", version_seven_config());
    FailingAtomicFileSystem files;
    files.fail_on_replace = 2U;
    RecordingAudit audit;
    paperbreak::config::ConfigRepository repository{path, files, audit};
    const auto loaded = repository.load();
    ASSERT_FALSE(loaded);
    EXPECT_EQ(loaded.error().business_code, "SYS_CONFIG_PERSIST_FAILED");
    std::ifstream original_stream{path, std::ios::binary};
    const std::string original{std::istreambuf_iterator<char>{original_stream},
                               std::istreambuf_iterator<char>{}};
    EXPECT_EQ(nlohmann::json::parse(original)["configSchemaVersion"], 7U);
}

TEST(ConfigRepository, AuditFailurePreventsModification)
{
    const TemporaryDirectory directory;
    const auto path = directory.write("config.json", valid_config());
    paperbreak::platform::WindowsAtomicFileSystem files;
    RecordingAudit audit;
    paperbreak::config::ConfigRepository repository{path, files, audit};
    ASSERT_TRUE(repository.load());
    audit.fail = true;
    const auto result =
        repository.update(replace_once(valid_config(), "\"fps\": 3.0", "\"fps\": 4.0"), 1U, {});
    ASSERT_FALSE(result);
    EXPECT_EQ(repository.snapshot().value().stored_config_revision, 1U);
}
