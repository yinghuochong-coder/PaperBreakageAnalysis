#include "paperbreak/uplink/qt_transport.hpp"
#include "paperbreak/uplink/simulator.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QUuid>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <thread>

namespace
{
using namespace std::chrono_literals;
using paperbreak::uplink::QtUplinkTransport;
using paperbreak::uplink::QtUplinkTransportConfig;
using paperbreak::uplink::simulator::Options;
using paperbreak::uplink::simulator::Runtime;

std::filesystem::path unique_path(const std::string_view prefix)
{
    return std::filesystem::temp_directory_path() / std::string{prefix} /
           QUuid::createUuid().toString(QUuid::WithoutBraces).toStdWString();
}

std::string server_url(const Runtime& runtime)
{
    return "http://127.0.0.1:" + std::to_string(runtime.snapshot().port);
}

std::string sha256_file(const std::filesystem::path& path)
{
    QFile file{QString::fromStdWString(path.wstring())};
    EXPECT_TRUE(file.open(QIODevice::ReadOnly));
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256)
        .toHex()
        .toStdString();
}

paperbreak::uplink::SessionHello hello(const std::string& request_id = "session-transport-1")
{
    return {.request_id = request_id,
            .machine_id = "EDGE-TRANSPORT",
            .production_line_id = "LINE-01",
            .software_version = "0.1.0",
            .supported_protocol_versions = {1U},
            .capabilities = {"system.requestStatus"}};
}

TEST(UplinkTransport, RejectsNonV1AndUnboundedConfiguration)
{
    EXPECT_FALSE(QtUplinkTransport::create({.server_url = "https://127.0.0.1:18080"}));
    EXPECT_FALSE(
        QtUplinkTransport::create({.server_url = "http://127.0.0.1:18080", .io_timeout = 99ms}));
    EXPECT_FALSE(QtUplinkTransport::create(
        {.server_url = "http://127.0.0.1:18080", .chunk_bytes = 4U * 1024U * 1024U + 1U}));
}

TEST(UplinkTransport, ConnectsWebSocketAcknowledgesAndDispatchesCommands)
{
    Runtime runtime;
    ASSERT_TRUE(runtime.start({.listen_address = "127.0.0.1",
                               .port = 0U,
                               .workspace = unique_path("PaperBreakTransportSession"),
                               .maximum_device_count = 16U,
                               .workspace_limit_bytes = 64ULL * 1024ULL * 1024ULL}));
    auto created = QtUplinkTransport::create({.server_url = server_url(runtime), .io_timeout = 3s});
    ASSERT_TRUE(created) << created.error().message;
    auto transport = std::move(created).value();
    std::atomic_uint32_t commands{};
    transport->set_command_handler([&commands](const auto&) { ++commands; });
    auto session = transport->connect(hello());
    ASSERT_TRUE(session) << session.error().message;
    EXPECT_EQ(session.value().machine_id, "EDGE-TRANSPORT");

    paperbreak::uplink::MessageEnvelope heartbeat{.protocol_version = 1U,
                                                  .message_type = "heartbeat",
                                                  .message_id = "heartbeat-transport-1",
                                                  .machine_id = "EDGE-TRANSPORT",
                                                  .sequence = 1U,
                                                  .timestamp = paperbreak::current_utc_timestamp(),
                                                  .payload_json = R"({"service":"Running"})"};
    auto acknowledged = transport->send_heartbeat(heartbeat);
    ASSERT_TRUE(acknowledged) << acknowledged.error().message;
    EXPECT_EQ(acknowledged.value().correlation_id, heartbeat.message_id);

    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (runtime.snapshot().devices.empty() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
    auto enqueued = runtime.enqueue_command({.command_id = "command-transport-1",
                                             .machine_id = "EDGE-TRANSPORT",
                                             .command_type = "system.requestStatus",
                                             .deadline = "2099-08-05T02:02:03.004Z"});
    ASSERT_TRUE(enqueued) << enqueued.error().message;
    while (commands.load() == 0U && std::chrono::steady_clock::now() < deadline)
    {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_EQ(commands.load(), 1U);
    transport->disconnect();
    runtime.stop();
}

TEST(UplinkTransport, DisconnectCancelsAnInFlightHttpWait)
{
    Runtime runtime;
    ASSERT_TRUE(runtime.start({.listen_address = "127.0.0.1",
                               .port = 0U,
                               .workspace = unique_path("PaperBreakTransportCancellation"),
                               .maximum_device_count = 16U,
                               .workspace_limit_bytes = 64ULL * 1024ULL * 1024ULL}));
    ASSERT_TRUE(runtime.set_fault_profile("EDGE-TRANSPORT", {.response_delay_ms = 60000U}));
    auto created =
        QtUplinkTransport::create({.server_url = server_url(runtime), .io_timeout = 60s});
    ASSERT_TRUE(created);
    std::shared_ptr<QtUplinkTransport> transport{std::move(created).value()};
    auto connecting = std::async(
        std::launch::async, [transport] { return transport->connect(hello("cancel-session")); });
    std::this_thread::sleep_for(50ms);
    const auto cancelled_at = std::chrono::steady_clock::now();
    transport->disconnect();
    ASSERT_NE(connecting.wait_for(1s), std::future_status::timeout);
    EXPECT_LT(std::chrono::steady_clock::now() - cancelled_at, 1s);
    EXPECT_FALSE(connecting.get());
    runtime.stop(1s);
}

TEST(UplinkTransport, UploadsMultipleChunksAndResumesServerCheckpointIdempotently)
{
    const auto workspace = unique_path("PaperBreakTransportUploadServer");
    const auto source_root = unique_path("PaperBreakTransportUploadSource");
    std::filesystem::create_directories(source_root / "event-1");
    const auto source = source_root / "event-1" / "raw.bin";
    {
        std::ofstream stream{source, std::ios::binary};
        const std::string block(96U * 1024U, 'P');
        stream.write(block.data(), static_cast<std::streamsize>(block.size()));
        stream.write("tail", 4);
    }

    Runtime runtime;
    ASSERT_TRUE(runtime.start({.listen_address = "127.0.0.1",
                               .port = 0U,
                               .workspace = workspace,
                               .maximum_device_count = 16U,
                               .workspace_limit_bytes = 64ULL * 1024ULL * 1024ULL}));
    auto created = QtUplinkTransport::create({.server_url = server_url(runtime),
                                              .io_timeout = 3s,
                                              .chunk_bytes = 64U * 1024U,
                                              .upload_limit_bytes_per_second = 256ULL * 1024ULL});
    ASSERT_TRUE(created) << created.error().message;
    std::shared_ptr<paperbreak::uplink::IUplinkTransport> transport{std::move(created).value()};
    ASSERT_TRUE(transport->connect(hello("session-transport-upload")));

    auto executor = paperbreak::uplink::make_chunked_upload_executor(
        transport,
        {.event_root = source_root, .machine_id = "EDGE-TRANSPORT", .chunk_bytes = 64U * 1024U});
    ASSERT_TRUE(executor) << executor.error().message;
    const paperbreak::storage::UploadJobRecord job{
        .job_id = 42,
        .idempotency_key = "EDGE-TRANSPORT-event-1-raw-1",
        .event_id = "event-1",
        .kind = paperbreak::storage::UploadJobKind::raw_file,
        .logical_id = "raw-1",
        .relative_path = "event-1/raw.bin",
        .payload_json = "{}",
        .checksum = "sha256:" + sha256_file(source),
        .upload_bytes = std::filesystem::file_size(source),
        .state = paperbreak::storage::UploadJobState::in_progress};
    const auto transfer_started = std::chrono::steady_clock::now();
    auto first = executor.value()(job, {});
    ASSERT_EQ(first.disposition, paperbreak::uplink::UploadAttemptDisposition::succeeded)
        << first.error_code;
    EXPECT_GE(std::chrono::steady_clock::now() - transfer_started, 250ms);
    EXPECT_NE(first.checkpoint_json.find("receivedChunks"), std::string::npos);
    auto duplicate = executor.value()(job, {});
    EXPECT_EQ(duplicate.disposition, paperbreak::uplink::UploadAttemptDisposition::succeeded);
    EXPECT_TRUE(
        std::filesystem::exists(workspace / "events" / "EDGE-TRANSPORT" / "event-1" / "raw-1"));
    transport->disconnect();
    runtime.stop();
    std::error_code ignored;
    std::filesystem::remove_all(source_root, ignored);
}

TEST(UplinkTransport, OfflineUploadReturnsBeforeAccessingSource)
{
    const auto source_root = unique_path("PaperBreakTransportChangedSource");
    std::filesystem::create_directories(source_root / "event-local");
    const auto source = source_root / "event-local" / "raw.bin";
    {
        std::ofstream stream{source, std::ios::binary};
        stream << "validated-source";
    }
    auto created = QtUplinkTransport::create({.server_url = "http://127.0.0.1:1"});
    ASSERT_TRUE(created);
    std::shared_ptr<paperbreak::uplink::IUplinkTransport> transport{std::move(created).value()};
    auto executor = paperbreak::uplink::make_chunked_upload_executor(
        transport,
        {.event_root = source_root, .machine_id = "EDGE-TRANSPORT", .chunk_bytes = 64U * 1024U});
    ASSERT_TRUE(executor);
    const paperbreak::storage::UploadJobRecord job{
        .job_id = 44,
        .idempotency_key = "EDGE-TRANSPORT-event-local-raw",
        .event_id = "event-local",
        .kind = paperbreak::storage::UploadJobKind::raw_file,
        .logical_id = "raw-local",
        .relative_path = "event-local/raw.bin",
        .payload_json = "{}",
        .checksum = std::string(64U, '0'),
        .upload_bytes = std::filesystem::file_size(source),
        .state = paperbreak::storage::UploadJobState::in_progress};
    ASSERT_TRUE(std::filesystem::remove(source));
    const auto result = executor.value()(job, {});
    EXPECT_EQ(result.disposition, paperbreak::uplink::UploadAttemptDisposition::retryable_failure);
    EXPECT_EQ(result.error_code, "UPLINK_DISCONNECTED");
    std::error_code ignored;
    std::filesystem::remove_all(source_root, ignored);
}

TEST(UplinkTransport, OnlineSourceHashChangeNeverCallsComplete)
{
    const auto workspace = unique_path("PaperBreakTransportChangedOnlineServer");
    const auto source_root = unique_path("PaperBreakTransportChangedOnlineSource");
    std::filesystem::create_directories(source_root / "event-changed");
    const auto source = source_root / "event-changed" / "raw.bin";
    {
        std::ofstream stream{source, std::ios::binary};
        stream << std::string(70U * 1024U, 'X');
    }
    Runtime runtime;
    ASSERT_TRUE(runtime.start({.listen_address = "127.0.0.1",
                               .port = 0U,
                               .workspace = workspace,
                               .maximum_device_count = 16U,
                               .workspace_limit_bytes = 64ULL * 1024ULL * 1024ULL}));
    auto created = QtUplinkTransport::create({.server_url = server_url(runtime), .io_timeout = 3s});
    ASSERT_TRUE(created);
    std::shared_ptr<paperbreak::uplink::IUplinkTransport> transport{std::move(created).value()};
    ASSERT_TRUE(transport->connect(hello("session-transport-changed-online")));
    auto executor = paperbreak::uplink::make_chunked_upload_executor(
        transport,
        {.event_root = source_root, .machine_id = "EDGE-TRANSPORT", .chunk_bytes = 64U * 1024U});
    ASSERT_TRUE(executor);
    const paperbreak::storage::UploadJobRecord job{
        .job_id = 46,
        .idempotency_key = "EDGE-TRANSPORT-event-changed-raw",
        .event_id = "event-changed",
        .kind = paperbreak::storage::UploadJobKind::raw_file,
        .logical_id = "raw-changed",
        .relative_path = "event-changed/raw.bin",
        .payload_json = "{}",
        .checksum = "sha256:" + std::string(64U, '0'),
        .upload_bytes = std::filesystem::file_size(source),
        .state = paperbreak::storage::UploadJobState::in_progress};

    const auto result = executor.value()(job, {});

    EXPECT_EQ(result.disposition,
              paperbreak::uplink::UploadAttemptDisposition::manual_intervention);
    EXPECT_EQ(result.error_code, "UPLOAD_SOURCE_CHANGED");
    EXPECT_NE(result.checkpoint_json.find("uploadId"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(workspace / "events" / "EDGE-TRANSPORT" / "event-changed" /
                                         "raw-changed"));
    transport->disconnect();
    runtime.stop();
    std::error_code ignored;
    std::filesystem::remove_all(source_root, ignored);
}

TEST(UplinkTransport, RetriesServerChecksumFailureWithoutLosingLogicalUpload)
{
    const auto workspace = unique_path("PaperBreakTransportChecksumServer");
    const auto source_root = unique_path("PaperBreakTransportChecksumSource");
    std::filesystem::create_directories(source_root / "event-checksum");
    const auto source = source_root / "event-checksum" / "keyframe.bin";
    {
        std::ofstream stream{source, std::ios::binary};
        stream << std::string(70U * 1024U, 'C');
    }
    Runtime runtime;
    ASSERT_TRUE(runtime.start({.listen_address = "127.0.0.1",
                               .port = 0U,
                               .workspace = workspace,
                               .maximum_device_count = 16U,
                               .workspace_limit_bytes = 64ULL * 1024ULL * 1024ULL}));
    auto created =
        QtUplinkTransport::create({.server_url = server_url(runtime),
                                   .io_timeout = 3s,
                                   .chunk_bytes = 64U * 1024U,
                                   .upload_limit_bytes_per_second = 64ULL * 1024ULL * 1024ULL});
    ASSERT_TRUE(created);
    std::shared_ptr<paperbreak::uplink::IUplinkTransport> transport{std::move(created).value()};
    ASSERT_TRUE(transport->connect(hello("session-transport-checksum")));
    ASSERT_TRUE(runtime.set_fault_profile("EDGE-TRANSPORT", {.force_checksum_mismatch = true}));
    auto executor = paperbreak::uplink::make_chunked_upload_executor(
        transport,
        {.event_root = source_root, .machine_id = "EDGE-TRANSPORT", .chunk_bytes = 64U * 1024U});
    ASSERT_TRUE(executor);
    paperbreak::storage::UploadJobRecord job{
        .job_id = 45,
        .idempotency_key = "EDGE-TRANSPORT-event-checksum-keyframe",
        .event_id = "event-checksum",
        .kind = paperbreak::storage::UploadJobKind::key_frame,
        .logical_id = "keyframe-checksum",
        .relative_path = "event-checksum/keyframe.bin",
        .payload_json = "{}",
        .checksum = "sha256:" + sha256_file(source),
        .upload_bytes = std::filesystem::file_size(source),
        .state = paperbreak::storage::UploadJobState::in_progress};
    auto failed = executor.value()(job, {});
    EXPECT_EQ(failed.disposition, paperbreak::uplink::UploadAttemptDisposition::retryable_failure);
    EXPECT_EQ(failed.error_code, "UPLOAD_CHECKSUM_MISMATCH");
    EXPECT_NE(failed.checkpoint_json.find("uploadId"), std::string::npos);
    job.checkpoint_json = failed.checkpoint_json;
    ASSERT_TRUE(runtime.set_fault_profile("EDGE-TRANSPORT", {}));
    auto retried = executor.value()(job, {});
    EXPECT_EQ(retried.disposition, paperbreak::uplink::UploadAttemptDisposition::succeeded)
        << retried.error_code;
    EXPECT_TRUE(std::filesystem::exists(workspace / "events" / "EDGE-TRANSPORT" / "event-checksum" /
                                        "keyframe-checksum"));
    transport->disconnect();
    runtime.stop();
    std::error_code ignored;
    std::filesystem::remove_all(source_root, ignored);
}

TEST(UplinkTransport, PreservesCheckpointAcrossInjectedChunkFailureAndResumes)
{
    const auto workspace = unique_path("PaperBreakTransportResumeServer");
    const auto source_root = unique_path("PaperBreakTransportResumeSource");
    std::filesystem::create_directories(source_root / "event-2");
    const auto source = source_root / "event-2" / "manifest.json";
    {
        std::ofstream stream{source, std::ios::binary};
        stream << std::string(150U * 1024U, 'R');
    }
    Runtime runtime;
    ASSERT_TRUE(runtime.start({.listen_address = "127.0.0.1",
                               .port = 0U,
                               .workspace = workspace,
                               .maximum_device_count = 16U,
                               .workspace_limit_bytes = 64ULL * 1024ULL * 1024ULL}));
    auto created =
        QtUplinkTransport::create({.server_url = server_url(runtime),
                                   .io_timeout = 3s,
                                   .chunk_bytes = 64U * 1024U,
                                   .upload_limit_bytes_per_second = 64ULL * 1024ULL * 1024ULL});
    ASSERT_TRUE(created);
    std::shared_ptr<paperbreak::uplink::IUplinkTransport> transport{std::move(created).value()};
    ASSERT_TRUE(transport->connect(hello("session-transport-resume")));
    ASSERT_TRUE(
        runtime.set_fault_profile("EDGE-TRANSPORT", {.disconnect_after_chunk = std::uint32_t{1U}}));
    auto executor = paperbreak::uplink::make_chunked_upload_executor(
        transport,
        {.event_root = source_root, .machine_id = "EDGE-TRANSPORT", .chunk_bytes = 64U * 1024U});
    ASSERT_TRUE(executor);
    paperbreak::storage::UploadJobRecord job{.job_id = 43,
                                             .idempotency_key = "EDGE-TRANSPORT-event-2-manifest",
                                             .event_id = "event-2",
                                             .kind = paperbreak::storage::UploadJobKind::manifest,
                                             .logical_id = "manifest-2",
                                             .relative_path = "event-2/manifest.json",
                                             .payload_json = "{}",
                                             .checksum = "sha256:" + sha256_file(source),
                                             .upload_bytes = std::filesystem::file_size(source),
                                             .state =
                                                 paperbreak::storage::UploadJobState::in_progress};
    auto interrupted = executor.value()(job, {});
    EXPECT_EQ(interrupted.disposition,
              paperbreak::uplink::UploadAttemptDisposition::retryable_failure)
        << interrupted.error_code;
    EXPECT_NE(interrupted.checkpoint_json.find("uploadId"), std::string::npos);
    job.checkpoint_json = interrupted.checkpoint_json;
    ASSERT_TRUE(runtime.set_fault_profile("EDGE-TRANSPORT", {}));
    auto resumed = executor.value()(job, {});
    EXPECT_EQ(resumed.disposition, paperbreak::uplink::UploadAttemptDisposition::succeeded)
        << resumed.error_code;
    EXPECT_TRUE(std::filesystem::exists(workspace / "events" / "EDGE-TRANSPORT" / "event-2" /
                                        "manifest-2"));
    transport->disconnect();
    runtime.stop();
    std::error_code ignored;
    std::filesystem::remove_all(source_root, ignored);
}

} // namespace
