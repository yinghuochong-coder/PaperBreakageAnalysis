#include "paperbreak/logging/logging.hpp"
#include "paperbreak/monitoring/monitoring.hpp"
#include "paperbreak/platform/atomic_file.hpp"
#include "paperbreak/pipeline/preview.hpp"
#include "paperbreak/service/system_commands.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
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

const paperbreak::ipc::PeerIdentity reader{
    .actor_sid = "S-1-5-21-reader", .local = true, .authenticated = true, .administrator = false};
const paperbreak::ipc::PeerIdentity administrator{
    .actor_sid = "S-1-5-21-admin", .local = true, .authenticated = true, .administrator = true};

} // namespace

TEST(SystemCommand, ReturnsBoundedStatusAndStructuredVersion)
{
    CommandFixture fixture;
    auto status = fixture.commands.handle(fixture.request("system.getStatus"), reader, {});
    ASSERT_TRUE(status);
    const Json status_json = Json::parse(status.value().payload_json);
    EXPECT_EQ(status_json.at("serviceState"), "running");
    EXPECT_TRUE(status_json.at("acceptingWrites").get<bool>());
    EXPECT_EQ(status_json.at("configSchemaVersion"), 1);
    EXPECT_EQ(status_json.at("storedConfigRevision"), 1);
    EXPECT_FALSE(status_json.at("machineId").get<std::string>().empty());

    auto version = fixture.commands.handle(fixture.request("system.getVersion"), reader, {});
    ASSERT_TRUE(version);
    const Json version_json = Json::parse(version.value().payload_json);
    EXPECT_FALSE(version_json.at("applicationVersion").get<std::string>().empty());
    EXPECT_TRUE(version_json.at("dependencies").contains("qt"));
}

TEST(SystemCommand, ValidatesPreviewSubscriptionAgainstBoundedRuntime)
{
    CommandFixture fixture;
    auto preview = std::make_shared<paperbreak::pipeline::PreviewRuntime>(
        std::vector<std::string>{"CAM01"}, paperbreak::pipeline::make_opencv_preview_encoder(),
        [](paperbreak::pipeline::PreviewDelivery) {});
    paperbreak::service::SystemCommandService commands(
        fixture.repository, fixture.status, {}, {}, {}, fixture.config_path.parent_path(), preview);
    const paperbreak::ipc::PeerIdentity preview_reader{
        .actor_sid = "S-1-5-21-preview", .connection_id = 42U, .local = true,
        .authenticated = true, .administrator = false};
    auto subscribed = commands.handle(fixture.request("preview.subscribe", R"({"cameraIds":["CAM01"]})"),
                                      preview_reader, {});
    ASSERT_TRUE(subscribed);
    EXPECT_TRUE(Json::parse(subscribed.value().payload_json).at("subscribed").get<bool>());
    EXPECT_EQ(preview->snapshot().subscriptions, 1U);

    auto invalid = commands.handle(fixture.request("preview.subscribe", R"({"cameraIds":["UNKNOWN"]})"),
                                   preview_reader, {});
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().business_code, "IPC_REQUEST_INVALID");

    auto unsubscribed = commands.handle(fixture.request("preview.unsubscribe"), preview_reader, {});
    ASSERT_TRUE(unsubscribed);
    EXPECT_EQ(preview->snapshot().subscriptions, 0U);
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

TEST(SystemCommand, RequiresElevatedAdministratorForReload)
{
    CommandFixture fixture;
    auto result = fixture.commands.handle(
        fixture.request("system.reloadConfig", R"({"expectedConfigRevision":1})"), reader, {});

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().business_code, "IPC_UNAUTHORIZED");
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
        fixture.request("system.reloadConfig", R"({"expectedConfigRevision":1})"), administrator,
        {});

    ASSERT_TRUE(result);
    const Json payload = Json::parse(result.value().payload_json);
    EXPECT_EQ(payload.at("storedConfigRevision"), 2);
    ASSERT_EQ(fixture.audit.records.size(), 1U);
    EXPECT_EQ(fixture.audit.records.front().source,
              paperbreak::config::ConfigChangeSource::local_ipc);
    EXPECT_EQ(fixture.audit.records.front().actor, administrator.actor_sid);
    EXPECT_EQ(fixture.audit.records.front().correlation_id, "019870f2-6c80-7a31-9b52-6e3b9ca1d88f");
}

TEST(SystemCommand, PreservesConfigConflictAndRejectsUnknownOrBinaryCommands)
{
    CommandFixture fixture;
    auto conflict = fixture.commands.handle(
        fixture.request("system.reloadConfig", R"({"expectedConfigRevision":42})"), administrator,
        {});
    ASSERT_FALSE(conflict);
    EXPECT_EQ(conflict.error().business_code, "SYS_CONFIG_VERSION_CONFLICT");

    auto unknown = fixture.commands.handle(fixture.request("camera.list"), reader, {});
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().business_code, "IPC_REQUEST_INVALID");

    auto binary_request = fixture.request("system.getStatus");
    binary_request.binary.push_back(std::byte{0x01});
    auto binary = fixture.commands.handle(binary_request, reader, {});
    ASSERT_FALSE(binary);
    EXPECT_EQ(binary.error().business_code, "IPC_REQUEST_INVALID");
}

TEST(SystemCommand, ReloadIsIdempotentAndInvalidConfigPreservesActiveSnapshot)
{
    CommandFixture fixture;
    auto unchanged = fixture.commands.handle(
        fixture.request("system.reloadConfig", R"({"expectedConfigRevision":1})"), administrator,
        {});
    ASSERT_TRUE(unchanged);
    const Json unchanged_payload = Json::parse(unchanged.value().payload_json);
    EXPECT_EQ(unchanged_payload.at("storedConfigRevision"), 1);
    EXPECT_EQ(unchanged_payload.at("effectiveConfigRevision"), 1);

    std::ofstream invalid{fixture.config_path, std::ios::trunc};
    invalid << R"({"configSchemaVersion":1})";
    invalid.close();
    auto rejected = fixture.commands.handle(
        fixture.request("system.reloadConfig", R"({"expectedConfigRevision":1})"), administrator,
        {});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().business_code, "SYS_CONFIG_INVALID");

    auto snapshot = fixture.repository.snapshot();
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().stored_config_revision, 1U);
    EXPECT_EQ(snapshot.value().effective_config_revision, 1U);
}

TEST(SystemCommand, QueriesMetricsAlarmsLogsAndRequiresAdministratorToAcknowledge)
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
    ASSERT_TRUE(logging->log(paperbreak::logging::Category::service,
                             paperbreak::logging::Level::warning, "recent marker"));
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
    auto denied =
        commands.handle(fixture.request("alarm.acknowledge", acknowledge_payload), reader, {});
    ASSERT_FALSE(denied);
    EXPECT_EQ(denied.error().business_code, "IPC_UNAUTHORIZED");
    auto acknowledged = commands.handle(fixture.request("alarm.acknowledge", acknowledge_payload),
                                        administrator, {});
    ASSERT_TRUE(acknowledged);
    EXPECT_TRUE(Json::parse(acknowledged.value().payload_json).at("acknowledged").get<bool>());

    auto logs = commands.handle(
        fixture.request("log.tail", R"({"categories":["service"],"limit":10})"), reader, {});
    ASSERT_TRUE(logs);
    const Json log_tail = Json::parse(logs.value().payload_json);
    ASSERT_EQ(log_tail.at("records").size(), 1U);
    EXPECT_EQ(log_tail.at("records").front().at("message"), "recent marker");

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
        {"log.tail", R"({"limit":0})"},
        {"alarm.acknowledge",
         acknowledge_payload.substr(0, acknowledge_payload.size() - 1U) + R"(,"extra":true})"}};
    for (const auto& [command, payload] : invalid_requests)
    {
        const auto result = commands.handle(fixture.request(command, payload), administrator, {});
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
