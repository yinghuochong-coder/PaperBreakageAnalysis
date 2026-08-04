#include "paperbreak/camera/control.hpp"
#include "paperbreak/camera/mock_camera.hpp"
#include "paperbreak/logging/logging.hpp"
#include "paperbreak/monitoring/monitoring.hpp"
#include "paperbreak/pipeline/preview.hpp"
#include "paperbreak/platform/atomic_file.hpp"
#include "paperbreak/service/system_commands.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
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
    const paperbreak::ipc::PeerIdentity preview_reader{.actor_sid = "S-1-5-21-preview",
                                                       .connection_id = 42U,
                                                       .local = true,
                                                       .authenticated = true,
                                                       .administrator = false};
    auto subscribed = commands.handle(
        fixture.request("preview.subscribe", R"({"cameraIds":["CAM01"]})"), preview_reader, {});
    ASSERT_TRUE(subscribed);
    EXPECT_TRUE(Json::parse(subscribed.value().payload_json).at("subscribed").get<bool>());
    EXPECT_EQ(preview->snapshot().subscriptions, 1U);

    auto invalid = commands.handle(
        fixture.request("preview.subscribe", R"({"cameraIds":["UNKNOWN"]})"), preview_reader, {});
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
    EXPECT_EQ(unknown.error().business_code, "SYS_NOT_SUPPORTED");

    auto binary_request = fixture.request("system.getStatus");
    binary_request.binary.push_back(std::byte{0x01});
    auto binary = fixture.commands.handle(binary_request, reader, {});
    ASSERT_FALSE(binary);
    EXPECT_EQ(binary.error().business_code, "IPC_REQUEST_INVALID");
}

TEST(SystemCommand, ControlsMockCameraPersistsConfigAndReturnsReadback)
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
                      {"gainDb", 2.0},
                      {"frameRate", 30.0},
                      {"roi", {{"width", 64}, {"height", 48}, {"offsetX", 0}, {"offsetY", 0}}},
                      {"pixelFormat", "Mono8"},
                      {"triggerMode", "Continuous"},
                      {"triggerSource", ""},
                      {"triggerDelayUs", 0},
                      {"packetSizeBytes", 1500},
                      {"interPacketDelayNs", 0}}});
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

    auto list = commands.handle(fixture.request("camera.list"), reader, {});
    ASSERT_TRUE(list);
    const Json listed = Json::parse(list.value().payload_json);
    ASSERT_EQ(listed["cameras"].size(), 1U);
    EXPECT_EQ(listed["storedConfigRevision"], 2U);
    EXPECT_TRUE(listed["topologyRestartRequired"].get<bool>());
    EXPECT_EQ(listed["cameras"][0]["state"], "disconnected");
    EXPECT_EQ(listed["cameras"][0]["saved"]["exposureUs"], 100.0);

    auto discovered = commands.handle(fixture.request("camera.discover"), reader, {});
    ASSERT_TRUE(discovered);
    const Json discovered_json = Json::parse(discovered.value().payload_json);
    ASSERT_EQ(discovered_json["devices"].size(), 1U);
    EXPECT_EQ(discovered_json["devices"][0]["networkInterface"], "loopback");
    EXPECT_TRUE(discovered_json["devices"][0]["exclusiveAccessAvailable"].get<bool>());
    auto denied =
        commands.handle(fixture.request("camera.connect", R"({"cameraId":"CAM01"})"), reader, {});
    ASSERT_FALSE(denied);
    EXPECT_EQ(denied.error().business_code, "IPC_UNAUTHORIZED");
    auto connected = commands.handle(fixture.request("camera.connect", R"({"cameraId":"CAM01"})"),
                                     administrator, {});
    ASSERT_TRUE(connected);
    const Json connected_json = Json::parse(connected.value().payload_json);
    EXPECT_EQ(connected_json["actual"]["exposureUs"], 100.0);
    EXPECT_EQ(connected_json["actual"]["pixelFormat"], "Mono8");
    auto readback =
        commands.handle(fixture.request("camera.getConfig", R"({"cameraId":"CAM01"})"), reader, {});
    ASSERT_TRUE(readback);
    EXPECT_EQ(Json::parse(readback.value().payload_json)["actual"]["frameRate"], 30.0);

    auto updated = commands.handle(
        fixture.request(
            "camera.updateConfig",
            R"({"cameraId":"CAM01","expectedConfigRevision":2,"parameters":{"exposureUs":120.0}})"),
        administrator, {});
    ASSERT_TRUE(updated);
    const Json update_json = Json::parse(updated.value().payload_json);
    EXPECT_TRUE(update_json["saved"].get<bool>());
    EXPECT_TRUE(update_json["dispatched"].get<bool>());
    EXPECT_TRUE(update_json["applied"].get<bool>());
    EXPECT_EQ(update_json["actual"]["exposureUs"], 120.0);

    auto software_mode = commands.handle(
        fixture.request(
            "camera.updateConfig",
            R"({"cameraId":"CAM01","expectedConfigRevision":3,"parameters":{"triggerMode":"Software","triggerSource":""}})"),
        administrator, {});
    ASSERT_TRUE(software_mode);
    EXPECT_EQ(Json::parse(software_mode.value().payload_json)["actual"]["triggerMode"], "Software");

    auto unsupported_roi = commands.handle(
        fixture.request(
            "camera.updateConfig",
            R"({"cameraId":"CAM01","expectedConfigRevision":4,"parameters":{"roi":{"width":65,"height":48,"offsetX":0,"offsetY":0}}})"),
        administrator, {});
    ASSERT_FALSE(unsupported_roi);
    EXPECT_EQ(unsupported_roi.error().business_code, "CAMERA_CONFIG_FAILED");
    ASSERT_TRUE(fixture.repository.snapshot());
    EXPECT_EQ(fixture.repository.snapshot().value().stored_config_revision, 4U);
    auto unchanged =
        commands.handle(fixture.request("camera.getConfig", R"({"cameraId":"CAM01"})"), reader, {});
    ASSERT_TRUE(unchanged);
    EXPECT_EQ(Json::parse(unchanged.value().payload_json)["actual"]["roi"]["width"], 64U);

    ASSERT_TRUE(commands.handle(fixture.request("camera.start", R"({"cameraId":"CAM01"})"),
                                administrator, {}));
    ASSERT_TRUE(commands.handle(
        fixture.request("camera.softwareTrigger", R"({"cameraId":"CAM01"})"), administrator, {}));
    auto capture = commands.handle(
        fixture.request("camera.captureSnapshot", R"({"cameraId":"CAM01"})"), administrator, {});
    ASSERT_TRUE(capture);
    EXPECT_EQ(Json::parse(capture.value().payload_json)["width"], 64U);
    ASSERT_TRUE(commands.handle(fixture.request("camera.stop", R"({"cameraId":"CAM01"})"),
                                administrator, {}));
    ASSERT_TRUE(commands.handle(fixture.request("camera.disconnect", R"({"cameraId":"CAM01"})"),
                                administrator, {}));

    auto extra = commands.handle(
        fixture.request("camera.getConfig", R"({"cameraId":"CAM01","extra":true})"), reader, {});
    ASSERT_FALSE(extra);
    EXPECT_EQ(extra.error().business_code, "IPC_REQUEST_INVALID");
    auto invalid_update = commands.handle(
        fixture.request(
            "camera.updateConfig",
            R"({"cameraId":"CAM01","expectedConfigRevision":4,"parameters":{"frameRate":0.0}})"),
        administrator, {});
    ASSERT_FALSE(invalid_update);
    EXPECT_EQ(invalid_update.error().business_code, "SYS_CONFIG_INVALID");
    ASSERT_TRUE(fixture.repository.snapshot());
    EXPECT_EQ(fixture.repository.snapshot().value().stored_config_revision, 4U);
    std::stop_source stopped;
    stopped.request_stop();
    auto stopping = commands.handle(fixture.request("camera.connect", R"({"cameraId":"CAM01"})"),
                                    administrator, stopped.get_token());
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
                      {"gainDb", 2.0},
                      {"frameRate", 30.0},
                      {"roi", {{"width", 65}, {"height", 48}, {"offsetX", 0}, {"offsetY", 0}}},
                      {"pixelFormat", "Mono8"},
                      {"triggerMode", "Continuous"},
                      {"triggerSource", ""},
                      {"triggerDelayUs", 0},
                      {"packetSizeBytes", 1500},
                      {"interPacketDelayNs", 0}}});
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

    auto connected = commands.handle(fixture.request("camera.connect", R"({"cameraId":"CAM01"})"),
                                     administrator, {});
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

TEST(SystemCommand, BindsDiscoveredApprovedCameraFromActualReadbackAndRequiresRestart)
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
    auto denied = commands.handle(fixture.request("camera.bind", request), reader, {});
    ASSERT_FALSE(denied);
    EXPECT_EQ(denied.error().business_code, "IPC_UNAUTHORIZED");

    auto bound = commands.handle(fixture.request("camera.bind", request), administrator, {});
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

    auto conflict = commands.handle(fixture.request("camera.bind", request), administrator, {});
    ASSERT_FALSE(conflict);
    EXPECT_EQ(conflict.error().business_code, "SYS_CONFIG_VERSION_CONFLICT");

    auto duplicate_slot = commands.handle(
        fixture.request(
            "camera.bind",
            R"({"cameraId":"CAM01","serialNumber":"OTHER","location":"出口","expectedConfigRevision":2})"),
        administrator, {});
    ASSERT_FALSE(duplicate_slot);
    EXPECT_EQ(duplicate_slot.error().business_code, "CAMERA_CONFIG_FAILED");
    auto duplicate_serial = commands.handle(
        fixture.request(
            "camera.bind",
            R"({"cameraId":"CAM02","serialNumber":"MOCK-BIND-01","location":"出口","expectedConfigRevision":2})"),
        administrator, {});
    ASSERT_FALSE(duplicate_serial);
    EXPECT_EQ(duplicate_serial.error().business_code, "CAMERA_CONFIG_FAILED");
    auto invalid_slot = commands.handle(
        fixture.request(
            "camera.bind",
            R"({"cameraId":"CAM05","serialNumber":"OTHER","location":"出口","expectedConfigRevision":2})"),
        administrator, {});
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
            administrator, {});
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
