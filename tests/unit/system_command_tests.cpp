#include "paperbreak/platform/atomic_file.hpp"
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
          commands(repository, status)
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
