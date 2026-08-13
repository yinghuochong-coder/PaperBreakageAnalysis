#include "paperbreak/camera/control.hpp"
#include "paperbreak/camera/mock_camera.hpp"
#include "paperbreak/logging/logging.hpp"
#include "paperbreak/monitoring/monitoring.hpp"
#include "paperbreak/pipeline/preview.hpp"
#include "paperbreak/platform/atomic_file.hpp"
#include "paperbreak/service/system_commands.hpp"
#include "paperbreak/storage/event_inspector.hpp"
#include "paperbreak/storage/event_store.hpp"
#include "paperbreak/storage/metadata_database.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace
{

using Json = nlohmann::json;

class RecordingAuditSink final : public paperbreak::config::IConfigAuditSink
{
  public:
    [[nodiscard]] paperbreak::Result<void> record(
        const paperbreak::config::ConfigAuditRecord& record) override
    {
        records.push_back(record);
        return paperbreak::Result<void>::success();
    }

    std::vector<paperbreak::config::ConfigAuditRecord> records;
};

class ScopedTempDirectory final
{
  public:
    ScopedTempDirectory()
    {
        static std::atomic_uint64_t sequence{0U};
        path = std::filesystem::temp_directory_path() /
               ("paperbreak-system-command-" + std::to_string(++sequence));
        std::filesystem::create_directories(path);
    }

    ~ScopedTempDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    std::filesystem::path path;
};

struct CommandFixture final
{
    CommandFixture()
        : config_path(temp.path / "edge-config.json"), repository(config_path, files, audit),
          status(std::make_shared<paperbreak::service::ServiceStatusStore>()),
          commands(repository, status, {}, {}, {}, config_path.parent_path())
    {
        const std::filesystem::path source =
            std::filesystem::path{PAPERBREAK_TEST_SOURCE_DIR} / "data" / "basic-config-valid.json";
        std::filesystem::copy_file(source, config_path,
                                   std::filesystem::copy_options::overwrite_existing);
        auto loaded = repository.load();
        if (!loaded)
        {
            throw std::runtime_error{loaded.error().message};
        }
        status->set_state(paperbreak::service::ServiceState::starting);
        status->set_state(paperbreak::service::ServiceState::running);
    }

    [[nodiscard]] paperbreak::ipc::RequestMessage request(std::string command,
                                                          std::string payload = "{}") const
    {
        return {.request_id = "019870f2-6c80-7a31-9b52-6e3b9ca1d88f",
                .command = std::move(command),
                .timestamp = "2026-08-01T12:00:00.123Z",
                .payload_json = std::move(payload),
                .binary = {}};
    }

    ScopedTempDirectory temp;
    std::filesystem::path config_path;
    paperbreak::platform::WindowsAtomicFileSystem files;
    RecordingAuditSink audit;
    paperbreak::config::ConfigRepository repository;
    std::shared_ptr<paperbreak::service::ServiceStatusStore> status;
    paperbreak::service::SystemCommandService commands;
};

class AlgorithmRuntimeConfigApplier final : public paperbreak::config::IConfigApplier
{
  public:
    explicit AlgorithmRuntimeConfigApplier(
        std::shared_ptr<paperbreak::service::EventRuntime> runtime)
        : runtime_(std::move(runtime))
    {
    }

    [[nodiscard]] std::string_view name() const noexcept override
    {
        return "algorithm-test-runtime";
    }

    [[nodiscard]] paperbreak::Result<void> prepare(const paperbreak::config::EdgeConfig& current,
                                                   const paperbreak::config::EdgeConfig& candidate,
                                                   const std::vector<std::string>&) override
    {
        previous_ = current;
        candidate_ = candidate;
        return paperbreak::Result<void>::success();
    }

    [[nodiscard]] paperbreak::Result<void> apply_and_readback(
        const paperbreak::config::EdgeConfig&) override
    {
        return runtime_->reconfigure(*candidate_);
    }

    [[nodiscard]] paperbreak::Result<void> commit(const paperbreak::config::EdgeConfig&) override
    {
        previous_.reset();
        candidate_.reset();
        return paperbreak::Result<void>::success();
    }

    [[nodiscard]] paperbreak::Result<void> rollback(
        const paperbreak::config::EdgeConfig& previous) noexcept override
    {
        auto result = runtime_->reconfigure(previous_.value_or(previous));
        previous_.reset();
        candidate_.reset();
        return result;
    }

  private:
    std::shared_ptr<paperbreak::service::EventRuntime> runtime_;
    std::optional<paperbreak::config::EdgeConfig> previous_;
    std::optional<paperbreak::config::EdgeConfig> candidate_;
};

const paperbreak::ipc::PeerIdentity reader{
    .actor_sid = "S-1-5-21-reader", .local = true, .authenticated = true, .administrator = false};

paperbreak::storage::EventPersistenceRequest command_event_request(const std::string& event_id)
{
    using namespace std::chrono_literals;
    using namespace paperbreak;
    using namespace paperbreak::camera;
    using namespace paperbreak::event;
    const auto wall = WallClockTime{std::chrono::sys_days{std::chrono::year{2026} / 8 / 4}};
    const auto make_frame = [&](const std::uint64_t sequence,
                                const std::chrono::milliseconds offset) {
        auto buffer = std::make_shared<FrameBuffer>(16U);
        for (std::size_t index = 0U; index < 16U; ++index)
            buffer->writable_bytes()[index] = static_cast<std::byte>(sequence + index);
        EXPECT_TRUE(buffer->set_size(16U));
        auto view = make_frame_view({.camera_id = "CAM01",
                                     .camera_frame_number = 100U + sequence,
                                     .sequence_number = sequence,
                                     .received_monotonic_time = MonotonicTime{offset},
                                     .received_wall_clock_time = wall + offset,
                                     .geometry = {.width = 4U, .height = 4U, .stride = 4U},
                                     .pixel_format = PixelFormat::mono8,
                                     .buffer = std::move(buffer)});
        EXPECT_TRUE(view);
        return std::move(view).value();
    };
    auto first = make_frame(1U, 100ms);
    auto second = make_frame(2U, 200ms);
    paperbreak::storage::EventPersistenceRequest request;
    request.metadata = {.event_id = event_id,
                        .event_state = "Candidate",
                        .candidate_time = wall + 200ms,
                        .start_time = wall + 100ms,
                        .end_time = wall + 300ms,
                        .camera_ids = {"CAM01"},
                        .trigger_camera_id = "CAM01",
                        .trigger_frame_number = 102U,
                        .trigger_reason = "ManualTest",
                        .confidence = 1.0,
                        .pre_event_duration = 100ms,
                        .post_event_duration = 100ms,
                        .algorithm_name = "mock-detector",
                        .algorithm_version = "m5",
                        .config_version = "1",
                        .machine_id = "EDGE-TEST",
                        .production_line_id = "PM-TEST",
                        .paper_type = "test",
                        .upload_state = "Pending",
                        .time_quality = "Normal"};
    request.window = {.event_id = event_id,
                      .version = 1U,
                      .requested_start = MonotonicTime{100ms},
                      .requested_end = MonotonicTime{300ms},
                      .closed_monotonic_time = MonotonicTime{301ms},
                      .display_wall_clock_time = wall + 200ms,
                      .camera_windows = {{.camera_id = "CAM01",
                                          .requested_start = MonotonicTime{100ms},
                                          .requested_end = MonotonicTime{300ms},
                                          .available_start = MonotonicTime{100ms},
                                          .available_end = MonotonicTime{200ms},
                                          .first_sequence_number = 1U,
                                          .last_sequence_number = 2U,
                                          .frames = {first, second},
                                          .complete = true}},
                      .complete = true};
    request.key_frames.push_back(
        {.descriptor = {.camera_id = "CAM01",
                        .camera_frame_number = second.camera_frame_number(),
                        .sequence_number = second.sequence_number(),
                        .monotonic_time = second.received_monotonic_time(),
                        .wall_clock_time = second.received_wall_clock_time(),
                        .geometry = second.geometry(),
                        .pixel_format = second.pixel_format(),
                        .reasons = {KeyFrameReason::candidate_trigger}},
         .jpeg = {std::byte{0xff}, std::byte{0xd8}, std::byte{0x01}, std::byte{0xff},
                  std::byte{0xd9}}});
    return request;
}

} // namespace

TEST(SystemCommand, ClassifiesOnlyKnownReadCommandsAsQueries)
{
    CommandFixture fixture;
    using ExecutionClass = paperbreak::ipc::IRequestHandler::ExecutionClass;
    EXPECT_EQ(fixture.commands.execution_class(fixture.request("camera.list")),
              ExecutionClass::read_only_query);
    EXPECT_EQ(fixture.commands.execution_class(fixture.request("system.getMetrics")),
              ExecutionClass::read_only_query);
    EXPECT_EQ(fixture.commands.execution_class(fixture.request("camera.start")),
              ExecutionClass::serial_control);
    EXPECT_EQ(fixture.commands.execution_class(fixture.request("future.unknown")),
              ExecutionClass::serial_control);
}

TEST(SystemCommand, ReturnsBoundedStatusAndStructuredVersion)
{
    CommandFixture fixture;
    auto status = fixture.commands.handle(fixture.request("system.getStatus"), reader, {});
    ASSERT_TRUE(status);
    const Json status_json = Json::parse(status.value().payload_json);
    EXPECT_EQ(status_json.at("serviceState"), "running");
    EXPECT_TRUE(status_json.at("acceptingWrites").get<bool>());
    EXPECT_EQ(status_json.at("configSchemaVersion"), 6);
    EXPECT_EQ(status_json.at("storedConfigRevision"), 1);
    EXPECT_FALSE(status_json.at("machineId").get<std::string>().empty());
    EXPECT_EQ(status_json.at("loggingLevel"), "info");

    auto version = fixture.commands.handle(fixture.request("system.getVersion"), reader, {});
    ASSERT_TRUE(version);
    const Json version_json = Json::parse(version.value().payload_json);
    EXPECT_FALSE(version_json.at("applicationVersion").get<std::string>().empty());
    EXPECT_TRUE(version_json.at("dependencies").contains("qt"));
}

TEST(SystemCommand, ReadsAndUpdatesCompleteStorageConfigurationWithRestartSemantics)
{
    CommandFixture fixture;
    auto read = fixture.commands.handle(fixture.request("storage.getConfig"), reader, {});
    ASSERT_TRUE(read) << read.error().message;
    const Json initial = Json::parse(read.value().payload_json);
    EXPECT_EQ(initial["storedConfigRevision"], 1U);
    EXPECT_EQ(initial["storage"]["maximumCacheStorageGiB"], 1000U);
    EXPECT_EQ(initial["effectiveStorage"], initial["storage"]);
    EXPECT_TRUE(initial["pendingRestartPaths"].empty());

    Json storage = initial["storage"];
    storage["eventRoot"] = "data/new-events";
    storage["rollingCacheEnabled"] = true;
    storage["warningFreeSpaceGiB"] = 210U;
    storage["criticalFreeSpaceGiB"] = 110U;
    storage["stopFreeSpaceGiB"] = 21U;
    storage["maximumEventStorageGiB"] = 900U;
    const Json update{{"expectedConfigRevision", 1U}, {"storage", storage}};
    auto updated =
        fixture.commands.handle(fixture.request("storage.updateConfig", update.dump()), reader, {});
    ASSERT_TRUE(updated) << updated.error().message;
    const Json response = Json::parse(updated.value().payload_json);
    EXPECT_EQ(response["storedConfigRevision"], 2U);
    EXPECT_EQ(response["effectiveConfigRevision"], 1U);
    EXPECT_FALSE(response["applied"].get<bool>());
    EXPECT_EQ(response["storage"]["eventRoot"], "data/new-events");
    EXPECT_TRUE(response["storage"]["rollingCacheEnabled"].get<bool>());
    EXPECT_NE(response["effectiveStorage"]["eventRoot"], "data/new-events");
    EXPECT_FALSE(response["effectiveStorage"]["rollingCacheEnabled"].get<bool>());
    EXPECT_EQ(response["effectiveStorage"]["warningFreeSpaceGiB"], 210U);
    EXPECT_EQ(response["effectiveStorage"]["maximumEventStorageGiB"], 900U);
    const auto pending = response["pendingRestartPaths"].get<std::vector<std::string>>();
    EXPECT_NE(std::ranges::find(pending, "/storage/roots"), pending.end());
    EXPECT_NE(std::ranges::find(pending, "/storage/nvme"), pending.end());
    ASSERT_EQ(fixture.audit.records.size(), 1U);
    EXPECT_EQ(fixture.audit.records.front().source,
              paperbreak::config::ConfigChangeSource::local_ipc);

    Json invalid_storage = response["storage"];
    invalid_storage["unexpected"] = true;
    auto invalid = fixture.commands.handle(
        fixture.request("storage.updateConfig",
                        Json{{"expectedConfigRevision", 2U}, {"storage", invalid_storage}}.dump()),
        reader, {});
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().business_code, "SYS_CONFIG_INVALID");
}

TEST(SystemCommand, ReadsAndUpdatesCompleteUplinkConfigurationWithRestartSemantics)
{
    CommandFixture fixture;
    auto read = fixture.commands.handle(fixture.request("uplink.getConfig"), reader, {});
    ASSERT_TRUE(read) << read.error().message;
    const Json initial = Json::parse(read.value().payload_json);
    EXPECT_EQ(initial["storedConfigRevision"], 1U);
    EXPECT_FALSE(initial["uplink"]["enabled"].get<bool>());
    EXPECT_EQ(initial["uplink"]["chunkBytes"], 1048576U);

    Json uplink = initial["uplink"];
    uplink["enabled"] = true;
    uplink["serverUrl"] = "http://192.0.2.20:18080";
    uplink["heartbeatSeconds"] = 7U;
    uplink["chunkBytes"] = 524288U;
    uplink["ioTimeoutMs"] = 12000U;
    uplink["uploadLimitMiBps"] = 40U;
    auto updated = fixture.commands.handle(
        fixture.request("uplink.updateConfig",
                        Json{{"expectedConfigRevision", 1U}, {"uplink", uplink}}.dump()),
        reader, {});
    ASSERT_TRUE(updated) << updated.error().message;
    const Json result = Json::parse(updated.value().payload_json);
    EXPECT_EQ(result["storedConfigRevision"], 2U);
    EXPECT_EQ(result["effectiveConfigRevision"], 1U);
    EXPECT_FALSE(result["applied"].get<bool>());
    EXPECT_EQ(result["pendingRestartPaths"], Json::array({"/uplink/transport"}));
    EXPECT_TRUE(result["uplink"]["enabled"].get<bool>());
    EXPECT_FALSE(result["effectiveUplink"]["enabled"].get<bool>());

    auto conflict = fixture.commands.handle(
        fixture.request("uplink.updateConfig",
                        Json{{"expectedConfigRevision", 1U}, {"uplink", uplink}}.dump()),
        reader, {});
    ASSERT_FALSE(conflict);
    EXPECT_EQ(conflict.error().business_code, "SYS_CONFIG_VERSION_CONFLICT");
    auto invalid = fixture.commands.handle(fixture.request("uplink.getConfig", R"({"extra":true})"),
                                           reader, {});
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().business_code, "IPC_REQUEST_INVALID");
}

TEST(SystemCommand, ConfiguresObservesAndTestsAlgorithmWithoutCreatingCandidate)
{
    using namespace std::chrono_literals;
    using namespace paperbreak;
    CommandFixture fixture;
    auto initial = fixture.repository.snapshot();
    ASSERT_TRUE(initial);
    Json document = Json::parse(config::serialize_config(*initial.value().stored));
    document["cameras"] =
        Json::array({{{"id", "CAM01"},
                      {"enabled", true},
                      {"serialNumber", "SIM-01"},
                      {"location", "test"},
                      {"exposureUs", 1000.0},
                      {"autoExposure", "Off"},
                      {"gainDb", 0.0},
                      {"frameRate", 10.0},
                      {"roi", {{"width", 4}, {"height", 4}, {"offsetX", 0}, {"offsetY", 0}}},
                      {"pixelFormat", "Mono8"},
                      {"triggerMode", "Continuous"},
                      {"triggerSource", "Off"},
                      {"triggerDelayUs", 0},
                      {"packetSizeBytes", 1500},
                      {"interPacketDelayNs", 0},
                      {"lineIo",
                       {{"alarmInputEnabled", false},
                        {"alarmActiveLevel", "High"},
                        {"strobeOutputEnabled", false},
                        {"strobeDurationUs", 0},
                        {"strobePreDelayUs", 0},
                        {"strobePostDelayUs", 0}}}}});
    document["acquisition"]["framePoolCapacity"] = 128U;
    document["preview"]["enabled"] = false;
    document["event"]["preEventSeconds"] = 1U;
    document["event"]["postEventSeconds"] = 0U;
    document["event"]["maxEventSeconds"] = 1U;
    document["event"]["mergeGapSeconds"] = 0U;
    auto prepared = fixture.repository.update(document.dump(), 1U,
                                              {.source = config::ConfigChangeSource::local_ipc,
                                               .actor = "test",
                                               .correlation_id = "algorithm-prepare"});
    ASSERT_TRUE(prepared) << prepared.error().message;
    config::ConfigRepository restarted_repository{fixture.config_path, fixture.files,
                                                  fixture.audit};
    auto restarted = restarted_repository.load();
    ASSERT_TRUE(restarted) << restarted.error().message;

    const auto event_root = fixture.temp.path / "algorithm-events";
    auto opened = storage::EventMetadataDatabase::open(
        {.database_path = fixture.temp.path / "algorithm-db" / "events.db",
         .event_root = event_root,
         .backup_directory = fixture.temp.path / "algorithm-backup"});
    ASSERT_TRUE(opened);
    std::shared_ptr<storage::EventMetadataDatabase> database{std::move(opened).value()};
    auto runtime = service::EventRuntime::create({.configuration = *prepared.value().stored,
                                                  .event_root = event_root,
                                                  .database = database});
    ASSERT_TRUE(runtime) << runtime.error().message;
    ASSERT_TRUE(runtime.value()->start());
    AlgorithmRuntimeConfigApplier applier{runtime.value()};
    ASSERT_TRUE(restarted_repository.register_applier(applier));
    service::SystemCommandService commands{
        restarted_repository, fixture.status, {}, {}, {}, fixture.config_path.parent_path(), {}, {},
        runtime.value()};

    auto observed = commands.handle(
        fixture.request("algorithm.getConfig", R"({"cameraId":"CAM01"})"), reader, {});
    ASSERT_TRUE(observed) << observed.error().message;
    const Json observed_json = Json::parse(observed.value().payload_json);
    EXPECT_EQ(observed_json["storedConfigRevision"], 2U);
    EXPECT_EQ(observed_json["runtime"]["state"], "disabled");
    EXPECT_FALSE(observed_json["runtime"]["hasCurrentFrame"].get<bool>());
    EXPECT_TRUE(observed_json["runtime"]["detector"].is_null());
    EXPECT_EQ(observed_json["runtime"]["metrics"]["consecutiveBacklogEvents"], 0U);
    EXPECT_EQ(observed_json["runtime"]["metrics"]["resultQueueRejected"], 0U);
    EXPECT_EQ(observed_json["runtime"]["metrics"]["queueCapacity"], 2U);
    EXPECT_EQ(observed_json["runtime"]["metrics"]["sampledSkippedFrames"], 0U);
    EXPECT_EQ(observed_json["runtime"]["metrics"]["missedProcessingSlots"], 0U);
    EXPECT_EQ(observed_json["runtime"]["metrics"]["configuredProcessingFps"], 60U);
    EXPECT_FALSE(observed_json["runtime"]["metrics"]["rearmPending"].get<bool>());
    EXPECT_EQ(observed_json["runtime"]["metrics"]["rearmSuppressedResults"], 0U);
    EXPECT_EQ(observed_json["algorithm"]["rearmDurationMs"], 500U);

    const Json algorithm{{"enabled", true},
                         {"type", "classical-vision"},
                         {"roi", {{"width", 4}, {"height", 4}, {"offsetX", 0}, {"offsetY", 0}}},
                         {"downsampleMode", "half"},
                         {"processingFps", 30},
                         {"candidateThreshold", 0.55},
                         {"confirmationThreshold", 0.85},
                         {"confirmationDurationMs", 120},
                         {"cooldownMs", 250},
                         {"rearmDurationMs", 750},
                         {"modelReference", ""},
                         {"modelVersion", "prototype-config"},
                         {"device", "cpu"},
                         {"debugOverlay", true}};
    const Json update{
        {"cameraId", "CAM01"}, {"expectedConfigRevision", 2U}, {"algorithm", algorithm}};
    auto updated =
        commands.handle(fixture.request("algorithm.updateConfig", update.dump()), reader, {});
    ASSERT_TRUE(updated) << updated.error().message;
    const Json updated_json = Json::parse(updated.value().payload_json);
    EXPECT_EQ(updated_json["storedConfigRevision"], 3U);
    EXPECT_EQ(updated_json["effectiveConfigRevision"], 3U);
    EXPECT_EQ(updated_json["runtime"]["configRevision"], 3U);
    EXPECT_EQ(updated_json["runtime"]["state"], "active");
    EXPECT_EQ(updated_json["runtime"]["detector"]["pluginId"], "classical-vision");
    EXPECT_TRUE(updated_json["runtime"]["detector"]["prototypeOnly"].get<bool>());

    auto buffer = std::make_shared<camera::FrameBuffer>(16U);
    std::ranges::fill(buffer->writable_bytes(), std::byte{0xff});
    ASSERT_TRUE(buffer->set_size(16U));
    auto current = camera::make_frame_view(
        {.camera_id = "CAM01",
         .camera_frame_number = 101U,
         .sequence_number = 1U,
         .received_monotonic_time = camera::MonotonicTime{100ms},
         .received_wall_clock_time =
             camera::WallClockTime{std::chrono::sys_days{std::chrono::year{2026} / 8 / 4}} + 100ms,
         .geometry = {.width = 4U, .height = 4U, .stride = 4U},
         .pixel_format = camera::PixelFormat::mono8,
         .buffer = std::move(buffer)});
    ASSERT_TRUE(current);
    ASSERT_TRUE(runtime.value()->submit_frame(std::move(current).value()));
    const auto candidates_before = runtime.value()->snapshot().candidates_created;

    auto tested = commands.handle(
        fixture.request("algorithm.testCurrentFrame", R"({"cameraId":"CAM01"})"), reader, {});
    ASSERT_TRUE(tested) << tested.error().message;
    const Json tested_json = Json::parse(tested.value().payload_json);
    EXPECT_TRUE(tested_json["isolated"].get<bool>());
    EXPECT_FALSE(tested_json["candidateCreated"].get<bool>());
    EXPECT_EQ(tested_json["detector"]["pluginId"], "classical-vision");
    EXPECT_EQ(tested_json["result"]["sequenceNumber"], 1U);
    EXPECT_FALSE(tested_json["result"]["debugMetrics"].empty());
    EXPECT_EQ(tested_json["previewFormat"], "jpeg");
    EXPECT_EQ(tested_json["previewSourceWidth"], 4U);
    EXPECT_EQ(tested_json["previewSourceHeight"], 4U);
    EXPECT_EQ(tested_json["previewBytes"], tested.value().binary.size());
    EXPECT_FALSE(tested.value().binary.empty());
    EXPECT_EQ(runtime.value()->snapshot().candidates_created, candidates_before);

    auto conflict =
        commands.handle(fixture.request("algorithm.updateConfig", update.dump()), reader, {});
    ASSERT_FALSE(conflict);
    EXPECT_EQ(conflict.error().business_code, "SYS_CONFIG_VERSION_CONFLICT");
    auto invalid = commands.handle(
        fixture.request("algorithm.getConfig", R"({"cameraId":"CAM01","extra":true})"), reader, {});
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().business_code, "IPC_REQUEST_INVALID");

    runtime.value()->request_stop();
    EXPECT_TRUE(runtime.value()->join(std::chrono::steady_clock::now() + 5s));
}

TEST(SystemCommand, ListsGetsReviewsExportsAndConfiguresCommittedEvents)
{
    CommandFixture fixture;
    const auto event_root =
        fixture.config_path.parent_path() / std::filesystem::path{u8"数据/事件 文件"};
    auto opened = paperbreak::storage::EventMetadataDatabase::open(
        {.database_path = fixture.temp.path / L"数据库" / L"events.db",
         .event_root = event_root,
         .backup_directory = fixture.temp.path / L"备份"});
    ASSERT_TRUE(opened);
    std::shared_ptr<paperbreak::storage::EventMetadataDatabase> database{std::move(opened).value()};
    auto writer = paperbreak::storage::EventTransactionWriter::create({.event_root = event_root});
    ASSERT_TRUE(writer);
    const std::string event_id = "019fcb3d-9999-7000-8000-000000000009";
    auto persisted = writer.value()->persist(command_event_request(event_id));
    ASSERT_TRUE(persisted);
    ASSERT_TRUE(database->index_committed_event(persisted.value().committed_directory));
    auto created_inspector =
        paperbreak::storage::EventInspector::create({.event_root = event_root});
    ASSERT_TRUE(created_inspector);
    std::shared_ptr<paperbreak::storage::EventInspector> inspector{
        std::move(created_inspector).value()};
    std::size_t reviewed_events = 0U;
    paperbreak::service::SystemCommandService commands{
        fixture.repository,
        fixture.status,
        {},
        {},
        {},
        fixture.config_path.parent_path(),
        {},
        {},
        {},
        database,
        inspector,
        [&reviewed_events](const auto&) { ++reviewed_events; }};

    const std::string collecting_id = "019fcb3d-aaaa-7000-8000-000000000010";
    ASSERT_TRUE(database->create_collecting_event({.event_id = collecting_id,
                                                   .decision_state = "Candidate",
                                                   .candidate_time_utc_ms = 1785801600000,
                                                   .start_time_utc_ms = 1785801599000,
                                                   .end_time_utc_ms = 1785801601000,
                                                   .camera_ids = {"CAM01"},
                                                   .trigger_camera_id = "CAM01",
                                                   .trigger_frame_number = 1U,
                                                   .trigger_reason = "Algorithm",
                                                   .confidence = 0.75,
                                                   .trigger_count = 1U}));
    auto collecting_detail = commands.handle(
        fixture.request("event.get", Json{{"eventId", collecting_id}}.dump()), reader, {});
    ASSERT_TRUE(collecting_detail);
    const auto collecting_json = Json::parse(collecting_detail.value().payload_json);
    EXPECT_EQ(collecting_json["event"]["persistenceState"], "Collecting");
    EXPECT_FALSE(collecting_json["event"]["artifactsAvailable"].get<bool>());
    EXPECT_TRUE(collecting_json["committedDirectory"].is_null());
    auto early_manifest = commands.handle(
        fixture.request("event.getManifest", Json{{"eventId", collecting_id}}.dump()), reader, {});
    ASSERT_FALSE(early_manifest);
    EXPECT_EQ(early_manifest.error().business_code, "EVENT_NOT_COMMITTED");
    auto early_review = commands.handle(
        fixture.request("event.confirm",
                        Json{{"eventId", collecting_id}, {"expectedReviewRevision", 1U}}.dump()),
        reader, {});
    ASSERT_FALSE(early_review);
    EXPECT_EQ(early_review.error().business_code, "EVENT_NOT_COMMITTED");

    auto list = commands.handle(
        fixture.request(
            "event.list",
            R"({"eventState":"Candidate","persistenceState":"Committed","cameraId":"CAM01","offset":0,"limit":1})"),
        reader, {});
    ASSERT_TRUE(list) << list.error().message;
    const Json listed = Json::parse(list.value().payload_json);
    EXPECT_EQ(listed["total"], 1U);
    ASSERT_EQ(listed["events"].size(), 1U);
    EXPECT_EQ(listed["events"][0]["eventId"], event_id);
    EXPECT_EQ(listed["events"][0]["decisionState"], "Candidate");
    EXPECT_EQ(listed["events"][0]["persistenceState"], "Committed");
    EXPECT_EQ(listed["events"][0]["reviewState"], "Unreviewed");
    EXPECT_EQ(listed["events"][0]["integrityState"], "Unverified");
    EXPECT_TRUE(listed["events"][0]["integrityCheckedAtUtcMs"].is_null());
    EXPECT_TRUE(listed["events"][0]["artifactsAvailable"].get<bool>());
    EXPECT_EQ(listed["events"][0]["reviewRevision"], 1U);
    EXPECT_TRUE(listed["events"][0]["thumbnailAvailable"].get<bool>());

    auto summary = commands.handle(
        fixture.request("event.getSummary", Json{{"eventId", event_id}}.dump()), reader, {});
    ASSERT_TRUE(summary) << summary.error().message;
    const Json summary_json = Json::parse(summary.value().payload_json);
    EXPECT_GT(summary_json["manifestBytes"].get<std::size_t>(), 0U);
    EXPECT_TRUE(summary_json["keyFramesTraceable"].get<bool>());
    EXPECT_EQ(summary_json["thumbnailBytes"], summary.value().binary.size());
    EXPECT_EQ(summary_json["event"]["integrityState"], "Unverified");
    EXPECT_FALSE(summary.value().binary.empty());
    auto after_summary = database->get_event(event_id);
    ASSERT_TRUE(after_summary);
    EXPECT_EQ(after_summary.value().integrity_state, "Unverified");

    auto structural_manifest = commands.handle(
        fixture.request("event.getManifest", Json{{"eventId", event_id}}.dump()), reader, {});
    ASSERT_TRUE(structural_manifest);
    const auto structural_header = Json::parse(structural_manifest.value().payload_json);
    EXPECT_FALSE(structural_header["verified"].get<bool>());
    EXPECT_EQ(structural_header["integrityState"], "Unverified");

    auto detail = commands.handle(fixture.request("event.get", Json{{"eventId", event_id}}.dump()),
                                  reader, {});
    ASSERT_TRUE(detail) << detail.error().message;
    const Json detail_json = Json::parse(detail.value().payload_json);
    EXPECT_GT(detail_json["manifestBytes"].get<std::size_t>(), 0U);
    EXPECT_TRUE(detail_json["keyFramesTraceable"].get<bool>());
    EXPECT_EQ(detail_json["thumbnailBytes"], detail.value().binary.size());
    EXPECT_EQ(detail_json["event"]["integrityState"], "Verified");
    EXPECT_FALSE(detail.value().binary.empty());
    auto manifest = commands.handle(
        fixture.request("event.getManifest", Json{{"eventId", event_id}}.dump()), reader, {});
    ASSERT_TRUE(manifest) << manifest.error().message;
    const Json manifest_header = Json::parse(manifest.value().payload_json);
    EXPECT_TRUE(manifest_header["verified"].get<bool>());
    const std::string manifest_text{reinterpret_cast<const char*>(manifest.value().binary.data()),
                                    manifest.value().binary.size()};
    EXPECT_EQ(Json::parse(manifest_text)["eventId"], event_id);

    const auto review_payload = Json{{"eventId", event_id}, {"expectedReviewRevision", 1U}}.dump();
    auto confirmed = commands.handle(fixture.request("event.confirm", review_payload), reader, {});
    ASSERT_TRUE(confirmed);
    const Json confirmed_json = Json::parse(confirmed.value().payload_json);
    EXPECT_EQ(confirmed_json["event"]["decisionState"], "Candidate");
    EXPECT_EQ(confirmed_json["event"]["reviewState"], "Reviewed");
    EXPECT_EQ(confirmed_json["event"]["reviewDecision"], "Confirmed");
    EXPECT_EQ(confirmed_json["event"]["reviewRevision"], 2U);
    EXPECT_EQ(reviewed_events, 1U);
    auto conflicting = commands.handle(fixture.request("event.reject", review_payload), reader, {});
    ASSERT_FALSE(conflicting);
    EXPECT_EQ(conflicting.error().business_code, "EVENT_VERSION_CONFLICT");

    auto exported = commands.handle(
        fixture.request("event.export", Json{{"eventId", event_id}}.dump()), reader, {});
    ASSERT_TRUE(exported) << exported.error().message;
    const auto export_json = Json::parse(exported.value().payload_json);
    EXPECT_EQ(export_json["verified"], true);
    EXPECT_TRUE(exported.value().binary.empty());
    const auto source_utf8 = export_json["exportSourcePath"].get<std::string>();
    std::u8string source_path_text;
    source_path_text.reserve(source_utf8.size());
    for (const unsigned char byte : source_utf8)
        source_path_text.push_back(static_cast<char8_t>(byte));
    std::ifstream archive{std::filesystem::path{source_path_text}, std::ios::binary};
    std::array<unsigned char, 2U> signature{};
    archive.read(reinterpret_cast<char*>(signature.data()),
                 static_cast<std::streamsize>(signature.size()));
    EXPECT_EQ(signature[0], 0x50U);
    EXPECT_EQ(signature[1], 0x4bU);

    auto config = commands.handle(fixture.request("event.getConfig"), reader, {});
    ASSERT_TRUE(config);
    Json config_json = Json::parse(config.value().payload_json);
    EXPECT_FALSE(config_json["uploadRuntimeAvailable"].get<bool>());
    config_json["event"]["preEventSeconds"] = 9U;
    auto updated = commands.handle(
        fixture.request(
            "event.updateConfig",
            Json{{"expectedConfigRevision", 1U}, {"event", config_json["event"]}}.dump()),
        reader, {});
    ASSERT_TRUE(updated) << updated.error().message;
    EXPECT_EQ(Json::parse(updated.value().payload_json)["event"]["preEventSeconds"], 9U);
    ASSERT_TRUE(
        database->enqueue_upload_job({.idempotency_key = "event-retry:" + event_id,
                                      .event_id = event_id,
                                      .kind = paperbreak::storage::UploadJobKind::manifest,
                                      .logical_id = "manifest",
                                      .relative_path = "2026/08/04/" + event_id + "/manifest.json",
                                      .payload_json = "{}",
                                      .checksum = "sha256",
                                      .upload_bytes = 1U,
                                      .created_at_utc_ms = 1}));
    auto upload = database->claim_next_upload_job(1);
    ASSERT_TRUE(upload);
    ASSERT_TRUE(upload.value());
    ASSERT_TRUE(database->fail_upload_job(
        upload.value()->job_id, paperbreak::storage::UploadFailureClass::manual_intervention,
        "UPLOAD_CHECKSUM_MISMATCH", "{}", std::nullopt, 2));
    auto retry = commands.handle(
        fixture.request("event.retryUpload", Json{{"eventId", event_id}}.dump()), reader, {});
    ASSERT_TRUE(retry);
    EXPECT_EQ(Json::parse(retry.value().payload_json)["requeuedJobs"], 1U);
    auto retried = database->get_upload_job("event-retry:" + event_id);
    ASSERT_TRUE(retried);
    ASSERT_TRUE(retried.value());
    EXPECT_EQ(retried.value()->state, paperbreak::storage::UploadJobState::pending);

    auto invalid = commands.handle(fixture.request("event.list", R"({"limit":0})"), reader, {});
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().business_code, "IPC_REQUEST_INVALID");
}

TEST(SystemCommand, ValidatesPreviewSubscriptionAgainstBoundedRuntime)
{
    CommandFixture fixture;
    auto preview = std::make_shared<paperbreak::pipeline::PreviewRuntime>(
        std::vector<std::string>{"CAM01"}, paperbreak::pipeline::make_opencv_preview_encoder(),
        [](paperbreak::pipeline::PreviewDelivery) {});
    paperbreak::service::SystemCommandService commands(
        fixture.repository, fixture.status, {}, {}, {}, fixture.config_path.parent_path(), preview);
    const paperbreak::ipc::PeerIdentity preview_reader{.actor_sid = "S-1-5-21-preview",
                                                       .connection_id = 42U,
                                                       .local = true,
                                                       .authenticated = true,
                                                       .administrator = false};
    auto subscribed =
        commands.handle(fixture.request("preview.subscribe", R"({"cameraIds":["CAM01"],"fps":30})"),
                        preview_reader, {});
    ASSERT_TRUE(subscribed);
    const auto subscribed_payload = Json::parse(subscribed.value().payload_json);
    EXPECT_TRUE(subscribed_payload.at("subscribed").get<bool>());
    EXPECT_EQ(subscribed_payload.at("fps").get<double>(), 30.0);
    EXPECT_EQ(preview->snapshot().subscriptions, 1U);

    auto invalid_fps = commands.handle(
        fixture.request("preview.subscribe", R"({"cameraIds":["CAM01"],"fps":30.1})"),
        preview_reader, {});
    ASSERT_FALSE(invalid_fps);
    EXPECT_EQ(invalid_fps.error().business_code, "IPC_REQUEST_INVALID");

    const paperbreak::ipc::PeerIdentity legacy_reader{.actor_sid = "S-1-5-21-preview-legacy",
                                                      .connection_id = 43U,
                                                      .local = true,
                                                      .authenticated = true,
                                                      .administrator = false};
    auto legacy = commands.handle(
        fixture.request("preview.subscribe", R"({"cameraIds":["CAM01"]})"), legacy_reader, {});
    ASSERT_TRUE(legacy);
    EXPECT_EQ(Json::parse(legacy.value().payload_json).at("fps").get<double>(), 3.0);

    auto invalid = commands.handle(
        fixture.request("preview.subscribe", R"({"cameraIds":["UNKNOWN"]})"), preview_reader, {});
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().business_code, "IPC_REQUEST_INVALID");

    auto unsubscribed = commands.handle(fixture.request("preview.unsubscribe"), preview_reader, {});
    ASSERT_TRUE(unsubscribed);
    EXPECT_EQ(preview->snapshot().subscriptions, 1U);
    preview->unsubscribe(legacy_reader.connection_id);
}

TEST(SystemCommand, ReturnsResolvedEventLocationAndRejectsFields)
{
    CommandFixture fixture;
    auto result = fixture.commands.handle(fixture.request("system.getLocations"), reader, {});
    ASSERT_TRUE(result);
    const Json payload = Json::parse(result.value().payload_json);
    ASSERT_EQ(payload.size(), 1U);
    const auto expected =
        (fixture.config_path.parent_path() / std::filesystem::path{u8"数据/事件 文件"})
            .lexically_normal();
    const std::u8string expected_utf8 = expected.generic_u8string();
    EXPECT_EQ(
        payload.at("eventRoot").get<std::string>(),
        std::string(reinterpret_cast<const char*>(expected_utf8.data()), expected_utf8.size()));

    auto invalid = fixture.commands.handle(
        fixture.request("system.getLocations", R"({"path":"C:/arbitrary"})"), reader, {});
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().business_code, "IPC_REQUEST_INVALID");
}

TEST(SystemCommand, AllowsAuthenticatedLocalNonAdministratorToReloadConfiguration)
{
    CommandFixture fixture;
    auto result = fixture.commands.handle(
        fixture.request("system.reloadConfig", R"({"expectedConfigRevision":1})"), reader, {});

    ASSERT_TRUE(result);
    EXPECT_EQ(Json::parse(result.value().payload_json).at("storedConfigRevision"), 1U);
}

TEST(SystemCommand, ReloadsThroughRepositoryAndRecordsPeerAuditContext)
{
    CommandFixture fixture;
    std::ifstream input{fixture.config_path};
    Json document = Json::parse(input);
    input.close();
    document["logging"]["level"] = "debug";
    std::ofstream output{fixture.config_path, std::ios::trunc};
    output << document.dump(2);
    output.close();

    auto result = fixture.commands.handle(
        fixture.request("system.reloadConfig", R"({"expectedConfigRevision":1})"), reader, {});

    ASSERT_TRUE(result);
    const Json payload = Json::parse(result.value().payload_json);
    EXPECT_EQ(payload.at("storedConfigRevision"), 2);
    ASSERT_EQ(fixture.audit.records.size(), 1U);
    EXPECT_EQ(fixture.audit.records.front().source,
              paperbreak::config::ConfigChangeSource::local_ipc);
    EXPECT_EQ(fixture.audit.records.front().actor, reader.actor_sid);
    EXPECT_EQ(fixture.audit.records.front().correlation_id, "019870f2-6c80-7a31-9b52-6e3b9ca1d88f");
}

TEST(SystemCommand, PreservesConfigConflictAndRejectsUnknownOrBinaryCommands)
{
    CommandFixture fixture;
    auto conflict = fixture.commands.handle(
        fixture.request("system.reloadConfig", R"({"expectedConfigRevision":42})"), reader, {});
    ASSERT_FALSE(conflict);
    EXPECT_EQ(conflict.error().business_code, "SYS_CONFIG_VERSION_CONFLICT");

    auto unknown = fixture.commands.handle(fixture.request("camera.list"), reader, {});
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().business_code, "SYS_NOT_SUPPORTED");

    auto binary_request = fixture.request("system.getStatus");
    binary_request.binary.push_back(std::byte{0x01});
    auto binary = fixture.commands.handle(binary_request, reader, {});
    ASSERT_FALSE(binary);
    EXPECT_EQ(binary.error().business_code, "IPC_REQUEST_INVALID");
}

TEST(SystemCommand, AllowsAuthenticatedLocalNonAdministratorToControlCamera)
{
    CommandFixture fixture;
    auto current = fixture.repository.snapshot();
    ASSERT_TRUE(current);
    Json document = Json::parse(paperbreak::config::serialize_config(*current.value().stored));
    document["cameras"] =
        Json::array({{{"id", "CAM01"},
                      {"enabled", true},
                      {"serialNumber", "MOCK-01"},
                      {"location", "测试位置"},
                      {"exposureUs", 100.0},
                      {"autoExposure", "Off"},
                      {"gainDb", 2.0},
                      {"frameRate", 30.0},
                      {"roi", {{"width", 64}, {"height", 48}, {"offsetX", 0}, {"offsetY", 0}}},
                      {"pixelFormat", "Mono8"},
                      {"triggerMode", "Continuous"},
                      {"triggerSource", ""},
                      {"triggerDelayUs", 0},
                      {"packetSizeBytes", 1500},
                      {"interPacketDelayNs", 0},
                      {"lineIo",
                       {{"alarmInputEnabled", false},
                        {"alarmActiveLevel", "High"},
                        {"strobeOutputEnabled", false},
                        {"strobeDurationUs", 0},
                        {"strobePreDelayUs", 0},
                        {"strobePostDelayUs", 0}}}}});
    ASSERT_TRUE(
        fixture.repository.update(document.dump(), 1U,
                                  {.source = paperbreak::config::ConfigChangeSource::local_ipc,
                                   .actor = "test",
                                   .correlation_id = "setup"}));

    auto provider = paperbreak::camera::mock::MockCameraProvider::create(
        {{.descriptor = {.model_name = "Mock",
                         .serial_number = "MOCK-01",
                         .ip_address = "127.0.0.1",
                         .network_interface = "loopback"},
          .width = 64U,
          .height = 48U,
          .frame_rate = 30.0}});
    ASSERT_TRUE(provider);
    auto mock_provider = std::move(provider).value();
    auto mock_control = mock_provider->control("MOCK-01");
    ASSERT_TRUE(mock_control);
    std::shared_ptr<paperbreak::camera::ICameraProvider> shared_provider{std::move(mock_provider)};
    auto runtime = std::make_shared<paperbreak::camera::CameraControlRuntime>(shared_provider);
    paperbreak::service::SystemCommandService commands{fixture.repository,
                                                       fixture.status,
                                                       {},
                                                       {},
                                                       {},
                                                       fixture.config_path.parent_path(),
                                                       {},
                                                       runtime};

    const paperbreak::ipc::PeerIdentity remote{.actor_sid = "S-1-5-21-remote",
                                               .local = false,
                                               .authenticated = true,
                                               .administrator = false};
    auto remote_denied = commands.handle(fixture.request("camera.list"), remote, {});
    ASSERT_FALSE(remote_denied);
    EXPECT_EQ(remote_denied.error().business_code, "IPC_UNAUTHORIZED");

    auto list = commands.handle(fixture.request("camera.list"), reader, {});
    ASSERT_TRUE(list);
    const Json listed = Json::parse(list.value().payload_json);
    ASSERT_EQ(listed["cameras"].size(), 1U);
    EXPECT_EQ(listed["storedConfigRevision"], 2U);
    EXPECT_TRUE(listed["topologyRestartRequired"].get<bool>());
    EXPECT_EQ(listed["cameras"][0]["state"], "disconnected");
    EXPECT_EQ(listed["cameras"][0]["saved"]["exposureUs"], 100.0);
    EXPECT_EQ(listed["cameras"][0]["saved"]["autoExposure"], "Off");

    auto discovered = commands.handle(fixture.request("camera.discover"), reader, {});
    ASSERT_TRUE(discovered);
    const Json discovered_json = Json::parse(discovered.value().payload_json);
    ASSERT_EQ(discovered_json["devices"].size(), 1U);
    EXPECT_EQ(discovered_json["devices"][0]["networkInterface"], "loopback");
    EXPECT_TRUE(discovered_json["devices"][0]["exclusiveAccessAvailable"].get<bool>());
    const paperbreak::ipc::PeerIdentity unauthenticated{
        .actor_sid = "", .local = true, .authenticated = false, .administrator = false};
    auto denied = commands.handle(fixture.request("camera.connect", R"({"cameraId":"CAM01"})"),
                                  unauthenticated, {});
    ASSERT_FALSE(denied);
    EXPECT_EQ(denied.error().business_code, "IPC_UNAUTHORIZED");
    auto connected =
        commands.handle(fixture.request("camera.connect", R"({"cameraId":"CAM01"})"), reader, {});
    ASSERT_TRUE(connected);
    const Json connected_json = Json::parse(connected.value().payload_json);
    EXPECT_EQ(connected_json["actual"]["exposureUs"], 100.0);
    EXPECT_EQ(connected_json["actual"]["autoExposure"], "Off");
    EXPECT_EQ(connected_json["actual"]["pixelFormat"], "Mono8");
    ASSERT_TRUE(connected_json.contains("capabilities"));
    EXPECT_EQ(connected_json["capabilities"]["roi"]["sensorWidth"], 64U);
    EXPECT_EQ(connected_json["capabilities"]["roi"]["sensorHeight"], 48U);
    EXPECT_EQ(connected_json["capabilities"]["roi"]["offsetY"]["increment"], 1U);
    EXPECT_TRUE(connected_json["capabilities"]["lineIo"]["alarmInputSupported"].get<bool>());
    EXPECT_TRUE(connected_json["capabilities"]["lineIo"]["risingEdgeSupported"].get<bool>());
    EXPECT_TRUE(connected_json["capabilities"]["lineIo"]["fallingEdgeSupported"].get<bool>());
    EXPECT_EQ(connected_json["capabilities"]["lineIo"]["strobeDurationUs"]["minimum"], 1U);
    auto readback =
        commands.handle(fixture.request("camera.getConfig", R"({"cameraId":"CAM01"})"), reader, {});
    ASSERT_TRUE(readback);
    EXPECT_EQ(Json::parse(readback.value().payload_json)["actual"]["frameRate"], 30.0);

    auto updated = commands.handle(
        fixture.request(
            "camera.updateConfig",
            R"({"cameraId":"CAM01","expectedConfigRevision":2,"parameters":{"exposureUs":120.0,"autoExposure":"Continuous","reverseX":true,"reverseY":true}})"),
        reader, {});
    ASSERT_TRUE(updated);
    const Json update_json = Json::parse(updated.value().payload_json);
    EXPECT_TRUE(update_json["saved"].get<bool>());
    EXPECT_TRUE(update_json["dispatched"].get<bool>());
    EXPECT_TRUE(update_json["applied"].get<bool>());
    EXPECT_EQ(update_json["actual"]["exposureUs"], 120.0);
    EXPECT_EQ(update_json["actual"]["autoExposure"], "Continuous");
    EXPECT_TRUE(update_json["actual"]["reverseX"].get<bool>());
    EXPECT_TRUE(update_json["actual"]["reverseY"].get<bool>());

    auto software_mode = commands.handle(
        fixture.request(
            "camera.updateConfig",
            R"({"cameraId":"CAM01","expectedConfigRevision":3,"parameters":{"triggerMode":"Software","triggerSource":""}})"),
        reader, {});
    ASSERT_TRUE(software_mode);
    EXPECT_EQ(Json::parse(software_mode.value().payload_json)["actual"]["triggerMode"], "Software");

    auto unsupported_roi = commands.handle(
        fixture.request(
            "camera.updateConfig",
            R"({"cameraId":"CAM01","expectedConfigRevision":4,"parameters":{"roi":{"width":65,"height":48,"offsetX":0,"offsetY":0}}})"),
        reader, {});
    ASSERT_FALSE(unsupported_roi);
    EXPECT_EQ(unsupported_roi.error().business_code, "CAMERA_CONFIG_FAILED");
    ASSERT_TRUE(fixture.repository.snapshot());
    EXPECT_EQ(fixture.repository.snapshot().value().stored_config_revision, 4U);
    auto unchanged =
        commands.handle(fixture.request("camera.getConfig", R"({"cameraId":"CAM01"})"), reader, {});
    ASSERT_TRUE(unchanged);
    EXPECT_EQ(Json::parse(unchanged.value().payload_json)["actual"]["roi"]["width"], 64U);

    auto line_io = commands.handle(
        fixture.request(
            "camera.updateConfig",
            R"({"cameraId":"CAM01","expectedConfigRevision":4,"parameters":{"lineIo":{"alarmInputEnabled":true,"alarmActiveLevel":"Low","strobeOutputEnabled":true,"strobeDurationUs":100,"strobePreDelayUs":10,"strobePostDelayUs":20}}})"),
        reader, {});
    ASSERT_TRUE(line_io) << line_io.error().message;
    const Json line_io_json = Json::parse(line_io.value().payload_json);
    EXPECT_TRUE(line_io_json["actual"]["lineIo"]["alarmInputEnabled"].get<bool>());
    EXPECT_EQ(line_io_json["actual"]["lineIo"]["strobeDurationUs"], 100U);
    ASSERT_TRUE(mock_control.value().set_line_input(false));
    Json low_active;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    do
    {
        const auto polled = commands.handle(fixture.request("camera.list"), reader, {});
        ASSERT_TRUE(polled);
        low_active = Json::parse(polled.value().payload_json)["cameras"][0];
        if (low_active.contains("lineInput") && low_active["lineInput"]["revision"] == 1U)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    } while (std::chrono::steady_clock::now() < deadline);
    ASSERT_TRUE(low_active.contains("lineInput"));
    EXPECT_FALSE(low_active["lineInput"]["rawLevel"].get<bool>());
    EXPECT_TRUE(low_active["lineInput"]["alarmActive"].get<bool>());
    EXPECT_FALSE(low_active["lineInput"]["stale"].get<bool>());

    ASSERT_TRUE(mock_control.value().set_line_input(true));
    const auto high_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    Json high_inactive;
    do
    {
        const auto polled = commands.handle(fixture.request("camera.list"), reader, {});
        ASSERT_TRUE(polled);
        high_inactive = Json::parse(polled.value().payload_json)["cameras"][0];
        if (high_inactive.contains("lineInput") && high_inactive["lineInput"]["revision"] == 2U)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    } while (std::chrono::steady_clock::now() < high_deadline);
    ASSERT_TRUE(high_inactive.contains("lineInput"));
    EXPECT_TRUE(high_inactive["lineInput"]["rawLevel"].get<bool>());
    EXPECT_FALSE(high_inactive["lineInput"]["alarmActive"].get<bool>());

    ASSERT_TRUE(
        commands.handle(fixture.request("camera.start", R"({"cameraId":"CAM01"})"), reader, {}));
    ASSERT_TRUE(commands.handle(
        fixture.request("camera.softwareTrigger", R"({"cameraId":"CAM01"})"), reader, {}));
    auto capture = commands.handle(
        fixture.request("camera.captureSnapshot", R"({"cameraId":"CAM01"})"), reader, {});
    ASSERT_TRUE(capture);
    EXPECT_EQ(Json::parse(capture.value().payload_json)["width"], 64U);
    ASSERT_TRUE(
        commands.handle(fixture.request("camera.stop", R"({"cameraId":"CAM01"})"), reader, {}));
    ASSERT_TRUE(commands.handle(fixture.request("camera.disconnect", R"({"cameraId":"CAM01"})"),
                                reader, {}));

    auto extra = commands.handle(
        fixture.request("camera.getConfig", R"({"cameraId":"CAM01","extra":true})"), reader, {});
    ASSERT_FALSE(extra);
    EXPECT_EQ(extra.error().business_code, "IPC_REQUEST_INVALID");
    auto invalid_update = commands.handle(
        fixture.request(
            "camera.updateConfig",
            R"({"cameraId":"CAM01","expectedConfigRevision":5,"parameters":{"frameRate":0.0}})"),
        reader, {});
    ASSERT_FALSE(invalid_update);
    EXPECT_EQ(invalid_update.error().business_code, "SYS_CONFIG_INVALID");
    ASSERT_TRUE(fixture.repository.snapshot());
    EXPECT_EQ(fixture.repository.snapshot().value().stored_config_revision, 5U);
    std::stop_source stopped;
    stopped.request_stop();
    auto stopping = commands.handle(fixture.request("camera.connect", R"({"cameraId":"CAM01"})"),
                                    reader, stopped.get_token());
    ASSERT_FALSE(stopping);
    EXPECT_EQ(stopping.error().business_code, "SYS_SERVICE_STOPPING");
}

TEST(SystemCommand, KeepsCameraConnectedWhenSavedParametersDoNotMatchDeviceCapabilities)
{
    CommandFixture fixture;
    auto current = fixture.repository.snapshot();
    ASSERT_TRUE(current);
    Json document = Json::parse(paperbreak::config::serialize_config(*current.value().stored));
    document["cameras"] =
        Json::array({{{"id", "CAM01"},
                      {"enabled", true},
                      {"serialNumber", "MOCK-MISMATCH-01"},
                      {"location", "测试位置"},
                      {"exposureUs", 100.0},
                      {"autoExposure", "Off"},
                      {"gainDb", 2.0},
                      {"frameRate", 30.0},
                      {"roi", {{"width", 65}, {"height", 48}, {"offsetX", 0}, {"offsetY", 0}}},
                      {"pixelFormat", "Mono8"},
                      {"triggerMode", "Continuous"},
                      {"triggerSource", ""},
                      {"triggerDelayUs", 0},
                      {"packetSizeBytes", 1500},
                      {"interPacketDelayNs", 0},
                      {"lineIo",
                       {{"alarmInputEnabled", false},
                        {"alarmActiveLevel", "High"},
                        {"strobeOutputEnabled", false},
                        {"strobeDurationUs", 0},
                        {"strobePreDelayUs", 0},
                        {"strobePostDelayUs", 0}}}}});
    ASSERT_TRUE(
        fixture.repository.update(document.dump(), 1U,
                                  {.source = paperbreak::config::ConfigChangeSource::local_ipc,
                                   .actor = "test",
                                   .correlation_id = "mismatch-setup"}));

    auto provider = paperbreak::camera::mock::MockCameraProvider::create(
        {{.descriptor = {.model_name = "Mock",
                         .serial_number = "MOCK-MISMATCH-01",
                         .ip_address = "127.0.0.1",
                         .network_interface = "loopback"},
          .width = 64U,
          .height = 48U,
          .frame_rate = 30.0}});
    ASSERT_TRUE(provider);
    std::shared_ptr<paperbreak::camera::ICameraProvider> shared_provider{
        std::move(provider).value()};
    auto runtime = std::make_shared<paperbreak::camera::CameraControlRuntime>(shared_provider);
    paperbreak::service::SystemCommandService commands{fixture.repository,
                                                       fixture.status,
                                                       {},
                                                       {},
                                                       {},
                                                       fixture.config_path.parent_path(),
                                                       {},
                                                       runtime};

    auto connected =
        commands.handle(fixture.request("camera.connect", R"({"cameraId":"CAM01"})"), reader, {});
    ASSERT_TRUE(connected);
    const Json response = Json::parse(connected.value().payload_json);
    EXPECT_EQ(response["state"], "connected");
    EXPECT_FALSE(response["applied"].get<bool>());
    EXPECT_EQ(response["applyError"]["code"], "CAMERA_CONFIG_FAILED");
    EXPECT_EQ(response["actual"]["roi"]["width"], 64U);

    auto readback =
        commands.handle(fixture.request("camera.getConfig", R"({"cameraId":"CAM01"})"), reader, {});
    ASSERT_TRUE(readback);
    EXPECT_EQ(Json::parse(readback.value().payload_json)["actual"]["roi"]["width"], 64U);

    auto listed = commands.handle(fixture.request("camera.list"), reader, {});
    ASSERT_TRUE(listed);
    EXPECT_EQ(Json::parse(listed.value().payload_json)["cameras"][0]["state"], "connected");
}

TEST(SystemCommand, AllowsAuthenticatedLocalNonAdministratorToBindApprovedCamera)
{
    CommandFixture fixture;
    auto provider = paperbreak::camera::mock::MockCameraProvider::create(
        {{.descriptor = {.model_name = "MV-CS020-60GM",
                         .serial_number = "MOCK-BIND-01",
                         .ip_address = "192.0.2.20",
                         .network_interface = "192.0.2.1",
                         .exclusive_access_available = true},
          .width = 64U,
          .height = 48U,
          .frame_rate = 30.0}});
    ASSERT_TRUE(provider);
    std::shared_ptr<paperbreak::camera::ICameraProvider> shared_provider{
        std::move(provider).value()};
    auto runtime = std::make_shared<paperbreak::camera::CameraControlRuntime>(shared_provider);
    paperbreak::service::SystemCommandService commands{fixture.repository,
                                                       fixture.status,
                                                       {},
                                                       {},
                                                       {},
                                                       fixture.config_path.parent_path(),
                                                       {},
                                                       runtime};

    const auto before = commands.handle(fixture.request("camera.list"), reader, {});
    ASSERT_TRUE(before);
    const Json before_json = Json::parse(before.value().payload_json);
    EXPECT_EQ(before_json["storedConfigRevision"], 1U);
    EXPECT_FALSE(before_json["topologyRestartRequired"].get<bool>());

    const std::string request =
        R"({"cameraId":"CAM01","serialNumber":"MOCK-BIND-01","location":"压榨部入口","expectedConfigRevision":1})";
    auto bound = commands.handle(fixture.request("camera.bind", request), reader, {});
    ASSERT_TRUE(bound);
    const Json response = Json::parse(bound.value().payload_json);
    EXPECT_TRUE(response["saved"].get<bool>());
    EXPECT_FALSE(response["applied"].get<bool>());
    EXPECT_TRUE(response["restartRequired"].get<bool>());
    EXPECT_EQ(response["storedConfigRevision"], 2U);

    const auto stored = fixture.repository.snapshot();
    ASSERT_TRUE(stored);
    ASSERT_EQ(stored.value().stored->cameras.size(), 1U);
    const auto& camera = stored.value().stored->cameras.front();
    EXPECT_EQ(camera.id, "CAM01");
    EXPECT_EQ(camera.serial_number, "MOCK-BIND-01");
    EXPECT_EQ(camera.location, "压榨部入口");
    EXPECT_DOUBLE_EQ(camera.exposure_us, 1000.0);
    EXPECT_DOUBLE_EQ(camera.frame_rate, 30.0);
    EXPECT_EQ(camera.roi.width, 64U);

    const auto after = commands.handle(fixture.request("camera.list"), reader, {});
    ASSERT_TRUE(after);
    const Json after_json = Json::parse(after.value().payload_json);
    EXPECT_EQ(after_json["storedConfigRevision"], 2U);
    EXPECT_TRUE(after_json["topologyRestartRequired"].get<bool>());
    EXPECT_EQ(after_json["cameras"].size(), 1U);

    auto conflict = commands.handle(fixture.request("camera.bind", request), reader, {});
    ASSERT_FALSE(conflict);
    EXPECT_EQ(conflict.error().business_code, "SYS_CONFIG_VERSION_CONFLICT");

    auto duplicate_slot = commands.handle(
        fixture.request(
            "camera.bind",
            R"({"cameraId":"CAM01","serialNumber":"OTHER","location":"出口","expectedConfigRevision":2})"),
        reader, {});
    ASSERT_FALSE(duplicate_slot);
    EXPECT_EQ(duplicate_slot.error().business_code, "CAMERA_CONFIG_FAILED");
    auto duplicate_serial = commands.handle(
        fixture.request(
            "camera.bind",
            R"({"cameraId":"CAM02","serialNumber":"MOCK-BIND-01","location":"出口","expectedConfigRevision":2})"),
        reader, {});
    ASSERT_FALSE(duplicate_serial);
    EXPECT_EQ(duplicate_serial.error().business_code, "CAMERA_CONFIG_FAILED");
    auto invalid_slot = commands.handle(
        fixture.request(
            "camera.bind",
            R"({"cameraId":"CAM05","serialNumber":"OTHER","location":"出口","expectedConfigRevision":2})"),
        reader, {});
    ASSERT_FALSE(invalid_slot);
    EXPECT_EQ(invalid_slot.error().business_code, "IPC_REQUEST_INVALID");
    ASSERT_TRUE(fixture.repository.snapshot());
    EXPECT_EQ(fixture.repository.snapshot().value().stored_config_revision, 2U);
    EXPECT_EQ(fixture.repository.snapshot().value().stored->cameras.size(), 1U);
}

TEST(SystemCommand, MissingCameraProviderReturnsDeploymentError)
{
    CommandFixture fixture;
    auto runtime = std::make_shared<paperbreak::camera::CameraControlRuntime>();
    paperbreak::service::SystemCommandService commands{fixture.repository,
                                                       fixture.status,
                                                       {},
                                                       {},
                                                       {},
                                                       fixture.config_path.parent_path(),
                                                       {},
                                                       runtime};

    auto list = commands.handle(fixture.request("camera.list"), reader, {});
    ASSERT_TRUE(list);
    EXPECT_TRUE(Json::parse(list.value().payload_json)["cameras"].empty());
    auto discovered = commands.handle(fixture.request("camera.discover"), reader, {});
    ASSERT_FALSE(discovered);
    EXPECT_EQ(discovered.error().business_code, "SYS_NOT_SUPPORTED");
    EXPECT_NE(discovered.error().message.find("部署完整性"), std::string::npos);
}

TEST(SystemCommand, RejectsOccupiedOrUnapprovedCameraBindingWithoutChangingConfiguration)
{
    const auto exercise = [](paperbreak::camera::CameraDeviceDescriptor descriptor,
                             const std::string& expected_code) {
        CommandFixture fixture;
        auto provider = paperbreak::camera::mock::MockCameraProvider::create(
            {{.descriptor = std::move(descriptor),
              .width = 64U,
              .height = 48U,
              .frame_rate = 30.0}});
        ASSERT_TRUE(provider);
        std::shared_ptr<paperbreak::camera::ICameraProvider> shared_provider{
            std::move(provider).value()};
        auto runtime = std::make_shared<paperbreak::camera::CameraControlRuntime>(shared_provider);
        paperbreak::service::SystemCommandService commands{fixture.repository,
                                                           fixture.status,
                                                           {},
                                                           {},
                                                           {},
                                                           fixture.config_path.parent_path(),
                                                           {},
                                                           runtime};
        auto result = commands.handle(
            fixture.request(
                "camera.bind",
                R"({"cameraId":"CAM01","serialNumber":"MOCK-BIND-02","location":"入口","expectedConfigRevision":1})"),
            reader, {});
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().business_code, expected_code);
        const auto stored = fixture.repository.snapshot();
        ASSERT_TRUE(stored);
        EXPECT_EQ(stored.value().stored_config_revision, 1U);
        EXPECT_TRUE(stored.value().stored->cameras.empty());
    };

    exercise({.model_name = "MV-CS020-60GM",
              .serial_number = "MOCK-BIND-02",
              .ip_address = "192.0.2.21",
              .network_interface = "192.0.2.1",
              .exclusive_access_available = false},
             "CAMERA_ACCESS_DENIED");
    exercise({.model_name = "UNAPPROVED",
              .serial_number = "MOCK-BIND-02",
              .ip_address = "192.0.2.21",
              .network_interface = "192.0.2.1",
              .exclusive_access_available = true},
             "CAMERA_CONFIG_FAILED");
}

TEST(SystemCommand, ReloadIsIdempotentAndInvalidConfigPreservesActiveSnapshot)
{
    CommandFixture fixture;
    auto unchanged = fixture.commands.handle(
        fixture.request("system.reloadConfig", R"({"expectedConfigRevision":1})"), reader, {});
    ASSERT_TRUE(unchanged);
    const Json unchanged_payload = Json::parse(unchanged.value().payload_json);
    EXPECT_EQ(unchanged_payload.at("storedConfigRevision"), 1);
    EXPECT_EQ(unchanged_payload.at("effectiveConfigRevision"), 1);

    std::ofstream invalid{fixture.config_path, std::ios::trunc};
    invalid << R"({"configSchemaVersion":1})";
    invalid.close();
    auto rejected = fixture.commands.handle(
        fixture.request("system.reloadConfig", R"({"expectedConfigRevision":1})"), reader, {});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().business_code, "SYS_CONFIG_INVALID");

    auto snapshot = fixture.repository.snapshot();
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().stored_config_revision, 1U);
    EXPECT_EQ(snapshot.value().effective_config_revision, 1U);
}

TEST(SystemCommand, AllowsAuthenticatedLocalNonAdministratorToAcknowledgeAlarm)
{
    CommandFixture fixture;
    auto metrics = std::make_shared<paperbreak::monitoring::MetricRegistry>();
    ASSERT_TRUE(metrics->replace_source(
        "system", {{.name = "process.cpu.percent", .value = 12.5, .unit = "percent"}}));
    auto alarms = std::make_shared<paperbreak::monitoring::AlarmRegistry>();
    auto raised = alarms->raise_alarm({.code = "SYS_CPU_USAGE_HIGH",
                                       .severity = paperbreak::Severity::warning,
                                       .source = "process",
                                       .message = "cpu high"});
    ASSERT_TRUE(raised);

    paperbreak::logging::LoggingConfig log_config;
    log_config.directory = fixture.temp.path / "logs";
    auto created = paperbreak::logging::LoggingRuntime::create(log_config);
    ASSERT_TRUE(created);
    std::shared_ptr<paperbreak::logging::LoggingRuntime> logging{std::move(created).value()};
    {
        auto registration = logging->register_current_thread("service-main");
        ASSERT_TRUE(registration);
        ASSERT_TRUE(logging->log(paperbreak::logging::Category::service,
                                 paperbreak::logging::Level::warning, "recent marker"));
    }
    ASSERT_TRUE(logging->shutdown());

    paperbreak::service::SystemCommandService commands{fixture.repository, fixture.status, metrics,
                                                       alarms, logging};
    auto metric_result = commands.handle(
        fixture.request("system.getMetrics", R"({"prefixes":["process."],"limit":10})"), reader,
        {});
    ASSERT_TRUE(metric_result);
    const Json metric_json = Json::parse(metric_result.value().payload_json);
    ASSERT_EQ(metric_json.at("metrics").size(), 1U);
    EXPECT_TRUE(metric_json.at("metrics").front().at("value").is_number_float());

    auto list_result =
        commands.handle(fixture.request("alarm.list", R"({"active":true,"limit":10})"), reader, {});
    ASSERT_TRUE(list_result);
    const Json alarm_list = Json::parse(list_result.value().payload_json);
    ASSERT_EQ(alarm_list.at("alarms").size(), 1U);
    EXPECT_FALSE(alarm_list.at("alarms").front().at("acknowledged").get<bool>());

    const std::string acknowledge_payload = Json{{"alarmId", raised.value().alarm_id}}.dump();
    auto acknowledged =
        commands.handle(fixture.request("alarm.acknowledge", acknowledge_payload), reader, {});
    ASSERT_TRUE(acknowledged);
    EXPECT_TRUE(Json::parse(acknowledged.value().payload_json).at("acknowledged").get<bool>());

    auto logs = commands.handle(
        fixture.request("log.tail",
                        R"({"categories":["service"],"threadName":"service-main","limit":10})"),
        reader, {});
    ASSERT_TRUE(logs);
    const Json log_tail = Json::parse(logs.value().payload_json);
    const auto marker = std::ranges::find_if(log_tail.at("records"), [](const auto& record) {
        return record.at("message") == "recent marker";
    });
    ASSERT_NE(marker, log_tail.at("records").end());
    EXPECT_EQ(marker->at("threadName"), "service-main");

    auto invalid = commands.handle(fixture.request("alarm.list", R"({"limit":201})"), reader, {});
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().business_code, "IPC_REQUEST_INVALID");

    const std::vector<std::pair<std::string, std::string>> invalid_requests{
        {"system.getMetrics", R"({"prefixes":"process."})"},
        {"system.getMetrics", R"({"unknown":1})"},
        {"alarm.list", R"({"active":"true"})"},
        {"alarm.list", R"({"minimumSeverity":"warning"})"},
        {"log.tail", R"({"afterSequence":-1})"},
        {"log.tail", R"({"categories":["unknown"]})"},
        {"log.tail", R"({"threadName":"Invalid_Name"})"},
        {"log.tail", R"({"limit":0})"},
        {"alarm.acknowledge",
         acknowledge_payload.substr(0, acknowledge_payload.size() - 1U) + R"(,"extra":true})"}};
    for (const auto& [command, payload] : invalid_requests)
    {
        const auto result = commands.handle(fixture.request(command, payload), reader, {});
        ASSERT_FALSE(result) << command << ' ' << payload;
        EXPECT_EQ(result.error().business_code, "IPC_REQUEST_INVALID");
    }

    ASSERT_TRUE(alarms->raise_alarm({.code = "SYS_MEMORY_USAGE_HIGH",
                                     .severity = paperbreak::Severity::warning,
                                     .source = "system",
                                     .message = "memory high"}));
    ASSERT_TRUE(alarms->raise_alarm({.code = "STORAGE_LOW_SPACE",
                                     .severity = paperbreak::Severity::warning,
                                     .source = "event",
                                     .message = "disk low"}));
    auto page = commands.handle(fixture.request("alarm.list", R"({"limit":2})"), reader, {});
    ASSERT_TRUE(page);
    const Json page_json = Json::parse(page.value().payload_json);
    EXPECT_EQ(page_json.at("alarms").size(), 2U);
    EXPECT_TRUE(page_json.at("truncated").get<bool>());
    EXPECT_TRUE(page_json.at("nextBeforeAlarmId").is_number_integer());
}

TEST(SystemCommand, ExportsBoundedZipWithRedactedConfigurationAndRecentDiagnostics)
{
    CommandFixture fixture;
    auto current = fixture.repository.snapshot();
    ASSERT_TRUE(current);
    Json document = Json::parse(paperbreak::config::serialize_config(*current.value().stored));
    document["uplink"]["credentialReference"] = "production-credential-reference";
    document["uplink"]["certificateReference"] = "production-certificate-reference";
    ASSERT_TRUE(
        fixture.repository.update(document.dump(), 1U,
                                  {.source = paperbreak::config::ConfigChangeSource::local_ipc,
                                   .actor = "test",
                                   .correlation_id = "diagnostic-setup"}));

    auto metrics = std::make_shared<paperbreak::monitoring::MetricRegistry>();
    ASSERT_TRUE(metrics->replace_source(
        "system",
        {{.name = "process.cpu.percent", .value = 17.5, .unit = "percent"},
         {.name = "algorithm.state", .value = std::string{"not-initialized"}, .unit = "state"}}));
    auto alarms = std::make_shared<paperbreak::monitoring::AlarmRegistry>();
    ASSERT_TRUE(alarms->raise_alarm({.code = "SYS_CPU_USAGE_HIGH",
                                     .severity = paperbreak::Severity::warning,
                                     .source = "process",
                                     .message = "diagnostic alarm"}));
    paperbreak::logging::LoggingConfig log_config;
    log_config.directory = fixture.temp.path / "diagnostic-logs";
    auto created = paperbreak::logging::LoggingRuntime::create(log_config);
    ASSERT_TRUE(created);
    std::shared_ptr<paperbreak::logging::LoggingRuntime> logging{std::move(created).value()};
    ASSERT_TRUE(logging->log(paperbreak::logging::Category::service,
                             paperbreak::logging::Level::warning,
                             "diagnostic marker token=super-secret-value"));
    ASSERT_TRUE(logging->shutdown());

    paperbreak::service::SystemCommandService commands{fixture.repository, fixture.status, metrics,
                                                       alarms, logging};
    auto exported = commands.handle(fixture.request("system.exportDiagnostics"), reader, {});
    ASSERT_TRUE(exported);
    ASSERT_GE(exported.value().binary.size(), 4U);
    EXPECT_EQ(exported.value().binary[0], std::byte{0x50});
    EXPECT_EQ(exported.value().binary[1], std::byte{0x4b});
    const Json response = Json::parse(exported.value().payload_json);
    EXPECT_EQ(response.at("contentType"), "application/zip");
    EXPECT_TRUE(response.at("redacted").get<bool>());
    EXPECT_EQ(response.at("size"), exported.value().binary.size());
    EXPECT_LT(exported.value().binary.size(), 8U * 1024U * 1024U);

    std::string bytes;
    bytes.reserve(exported.value().binary.size());
    for (const auto value : exported.value().binary)
        bytes.push_back(static_cast<char>(std::to_integer<unsigned char>(value)));
    const auto signature_count = [&bytes](const std::string_view signature) {
        std::size_t count = 0U;
        for (std::size_t offset = 0U; (offset = bytes.find(signature, offset)) != std::string::npos;
             offset += signature.size())
            ++count;
        return count;
    };
    EXPECT_EQ(signature_count(std::string_view{"PK\x03\x04", 4U}), 9U);
    EXPECT_EQ(signature_count(std::string_view{"PK\x01\x02", 4U}), 9U);
    EXPECT_EQ(signature_count(std::string_view{"PK\x05\x06", 4U}), 1U);
    for (const std::string_view entry :
         {"manifest.json", "config-redacted.json", "system.json", "metrics.json", "cameras.json",
          "network.json", "alarms.json", "recent-logs.json", "version.json"})
        EXPECT_NE(bytes.find(entry), std::string::npos) << entry;
    EXPECT_NE(bytes.find("<redacted>"), std::string::npos);
    EXPECT_NE(bytes.find("diagnostic alarm"), std::string::npos);
    EXPECT_NE(bytes.find("diagnostic marker token=***"), std::string::npos);
    EXPECT_EQ(bytes.find("production-credential-reference"), std::string::npos);
    EXPECT_EQ(bytes.find("production-certificate-reference"), std::string::npos);
    EXPECT_EQ(bytes.find("super-secret-value"), std::string::npos);

    auto invalid = commands.handle(
        fixture.request("system.exportDiagnostics", R"({"unexpected":true})"), reader, {});
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().business_code, "IPC_REQUEST_INVALID");
    const paperbreak::ipc::PeerIdentity unauthenticated{
        .actor_sid = "", .local = true, .authenticated = false, .administrator = false};
    auto denied = commands.handle(fixture.request("system.exportDiagnostics"), unauthenticated, {});
    ASSERT_FALSE(denied);
    EXPECT_EQ(denied.error().business_code, "IPC_UNAUTHORIZED");
}

TEST(SystemCommand, UplinkCommandsReuseStatusValidationAndRequireAuditForMutations)
{
    CommandFixture fixture;
    paperbreak::uplink::RemoteCommand status_command{.command_id = "remote-status-1",
                                                     .command_type = "system.requestStatus",
                                                     .deadline = "2999-01-01T00:00:00.000Z",
                                                     .operator_confirmed = false,
                                                     .body_json = "{}"};
    auto status = fixture.commands.handle_uplink_command(status_command, {});
    ASSERT_TRUE(status);
    EXPECT_EQ(Json::parse(status.value())["serviceState"], "running");

    const auto snapshot = fixture.repository.snapshot();
    ASSERT_TRUE(snapshot);
    Json candidate = Json::parse(paperbreak::config::serialize_config(*snapshot.value().stored));
    candidate["preview"]["fps"] = 4.0;
    paperbreak::uplink::RemoteCommand replace{
        .command_id = "remote-config-1",
        .command_type = "config.replace",
        .deadline = "2999-01-01T00:00:00.000Z",
        .operator_confirmed = true,
        .body_json = Json{{"expectedConfigRevision", 1U}, {"config", candidate}}.dump()};
    auto no_audit = fixture.commands.handle_uplink_command(replace, {});
    ASSERT_FALSE(no_audit);
    EXPECT_EQ(no_audit.error().business_code, "SYS_NOT_SUPPORTED");
    ASSERT_TRUE(fixture.repository.snapshot());
    EXPECT_EQ(fixture.repository.snapshot().value().stored_config_revision, 1U);
}

TEST(SystemCommand, UplinkConfigReplaceUsesUplinkAuditSourceAndExistingSchemaChecks)
{
    CommandFixture fixture;
    paperbreak::logging::LoggingConfig log_config;
    log_config.directory = fixture.temp.path / "uplink-audit";
    auto created = paperbreak::logging::LoggingRuntime::create(log_config);
    ASSERT_TRUE(created);
    std::shared_ptr<paperbreak::logging::LoggingRuntime> logging{std::move(created).value()};
    paperbreak::service::SystemCommandService commands{
        fixture.repository, fixture.status, {}, {}, logging, fixture.config_path.parent_path()};

    const auto before = fixture.repository.snapshot();
    ASSERT_TRUE(before);
    Json candidate = Json::parse(paperbreak::config::serialize_config(*before.value().stored));
    candidate["preview"]["fps"] = 4.0;
    paperbreak::uplink::RemoteCommand replace{
        .command_id = "remote-config-2",
        .command_type = "config.replace",
        .deadline = "2999-01-01T00:00:00.000Z",
        .operator_confirmed = true,
        .body_json = Json{{"expectedConfigRevision", 1U}, {"config", candidate}}.dump()};
    auto updated = commands.handle_uplink_command(replace, {});
    ASSERT_TRUE(updated);
    EXPECT_EQ(Json::parse(updated.value())["storedConfigRevision"], 2U);
    ASSERT_EQ(fixture.audit.records.size(), 1U);
    EXPECT_EQ(fixture.audit.records.front().source, paperbreak::config::ConfigChangeSource::uplink);
    EXPECT_EQ(fixture.audit.records.front().actor, "uplink:remote-config-2");
    EXPECT_EQ(fixture.audit.records.front().correlation_id, "remote-config-2");

    candidate["preview"]["fps"] = 1000.0;
    candidate["configRevision"] = 2U;
    paperbreak::uplink::RemoteCommand invalid{
        .command_id = "remote-config-invalid",
        .command_type = "config.replace",
        .deadline = "2999-01-01T00:00:00.000Z",
        .operator_confirmed = true,
        .body_json = Json{{"expectedConfigRevision", 2U}, {"config", candidate}}.dump()};
    auto rejected = commands.handle_uplink_command(invalid, {});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().business_code, "SYS_CONFIG_INVALID");
    ASSERT_TRUE(fixture.repository.snapshot());
    EXPECT_EQ(fixture.repository.snapshot().value().stored_config_revision, 2U);

    ASSERT_TRUE(logging->shutdown());
    const auto audit_logs =
        logging->tail({.categories = {paperbreak::logging::Category::audit}, .limit = 20U});
    EXPECT_GE(audit_logs.records.size(), 4U);
    EXPECT_NE(std::ranges::find_if(audit_logs.records,
                                   [](const auto& record) {
                                       return record.message.find("remote-config-2 success=true") !=
                                              std::string::npos;
                                   }),
              audit_logs.records.end());
    EXPECT_NE(std::ranges::find_if(audit_logs.records,
                                   [](const auto& record) {
                                       return record.message.find(
                                                  "remote-config-invalid success=false") !=
                                              std::string::npos;
                                   }),
              audit_logs.records.end());
}

TEST(SystemCommand, UplinkEventAndCameraMappingsKeepExistingValidationAndConfirmation)
{
    CommandFixture fixture;
    paperbreak::logging::LoggingConfig log_config;
    log_config.directory = fixture.temp.path / "uplink-command-audit";
    auto created = paperbreak::logging::LoggingRuntime::create(log_config);
    ASSERT_TRUE(created);
    std::shared_ptr<paperbreak::logging::LoggingRuntime> logging{std::move(created).value()};
    paperbreak::service::SystemCommandService commands{
        fixture.repository, fixture.status, {}, {}, logging, fixture.config_path.parent_path()};
    paperbreak::uplink::RemoteCommand unconfirmed{.command_id = "remote-camera-1",
                                                  .command_type = "camera.start",
                                                  .deadline = "2999-01-01T00:00:00.000Z",
                                                  .operator_confirmed = false,
                                                  .body_json = R"({"cameraId":"CAM01"})"};
    auto denied = commands.handle_uplink_command(unconfirmed, {});
    ASSERT_FALSE(denied);
    EXPECT_EQ(denied.error().business_code, "UPLINK_COMMAND_NOT_CONFIRMED");

    unconfirmed.operator_confirmed = true;
    auto camera = commands.handle_uplink_command(unconfirmed, {});
    ASSERT_FALSE(camera);
    EXPECT_EQ(camera.error().business_code, "SYS_NOT_SUPPORTED");

    paperbreak::uplink::RemoteCommand review{
        .command_id = "remote-review-1",
        .command_type = "event.review",
        .deadline = "2999-01-01T00:00:00.000Z",
        .operator_confirmed = true,
        .body_json = R"({"eventId":"event-1","expectedReviewRevision":1,"decision":"invalid"})"};
    auto invalid_review = commands.handle_uplink_command(review, {});
    ASSERT_FALSE(invalid_review);
    EXPECT_EQ(invalid_review.error().business_code, "IPC_REQUEST_INVALID");
    ASSERT_TRUE(logging->shutdown());
}
