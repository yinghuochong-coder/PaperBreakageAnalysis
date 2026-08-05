#include "paperbreak/uplink/protocol.hpp"
#include "paperbreak/uplink/simulator.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QWebSocket>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace
{
using Json = nlohmann::json;
using paperbreak::uplink::simulator::Options;
using paperbreak::uplink::simulator::Runtime;

struct HttpResult final
{
    int status{};
    QByteArray body;
};

HttpResult request(const QNetworkAccessManager::Operation operation, const QUrl& url,
                   const QByteArray& body = {}, const QByteArray& chunk_sha256 = {},
                   const QByteArray& content_range = {})
{
    QNetworkAccessManager manager;
    QNetworkRequest network_request(url);
    network_request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (!chunk_sha256.isEmpty())
        network_request.setRawHeader("x-chunk-sha256", chunk_sha256);
    if (!content_range.isEmpty())
        network_request.setRawHeader("content-range", content_range);
    QNetworkReply* reply = nullptr;
    if (operation == QNetworkAccessManager::GetOperation)
        reply = manager.get(network_request);
    else if (operation == QNetworkAccessManager::PostOperation)
        reply = manager.post(network_request, body);
    else if (operation == QNetworkAccessManager::PutOperation)
        reply = manager.put(network_request, body);
    if (reply == nullptr)
        return {};
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(5000);
    loop.exec();
    if (!reply->isFinished())
    {
        reply->abort();
        reply->deleteLater();
        return {.status = 599};
    }
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray response = reply->readAll();
    reply->deleteLater();
    return {.status = status, .body = response};
}

std::filesystem::path unique_workspace()
{
    return std::filesystem::temp_directory_path() / "PaperBreakUplinkSimulatorTests" /
           QUuid::createUuid().toString(QUuid::WithoutBraces).toStdWString();
}

QUrl base_url(const Runtime& runtime, const QString& path)
{
    return QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(runtime.snapshot().port).arg(path));
}

std::string digest(const QByteArray& bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toStdString();
}

bool wait_until(const std::function<bool()>& predicate,
                const std::chrono::milliseconds timeout = std::chrono::seconds{5})
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return predicate();
}

TEST(UplinkSimulator, ParsesBoundedVersionedFaultScenario)
{
    auto parsed = paperbreak::uplink::simulator::parse_scenario(R"({
        "schemaVersion":1,
        "devices":{"EDGE-01":{"responseDelayMs":10,"failNextRequests":2,
        "duplicateAcknowledgements":true,"replayCommands":true,"disconnectAfterChunk":3}}
    })");
    ASSERT_TRUE(parsed) << parsed.error().message;
    ASSERT_EQ(parsed.value().size(), 1U);
    EXPECT_EQ(parsed.value().front().first, "EDGE-01");
    EXPECT_EQ(parsed.value().front().second.response_delay_ms, 10U);
    EXPECT_TRUE(parsed.value().front().second.replay_commands);
    EXPECT_EQ(parsed.value().front().second.disconnect_after_chunk, 3U);

    auto unknown = paperbreak::uplink::simulator::parse_scenario(
        R"({"schemaVersion":1,"devices":{"EDGE-01":{"unknown":true}}})");
    EXPECT_FALSE(unknown);
}

TEST(UplinkSimulator, ServesSessionWebSocketPreviewAndPersistentResumableUpload)
{
    const auto workspace = unique_workspace();
    Runtime runtime;
    auto started = runtime.start(Options{.listen_address = "127.0.0.1",
                                         .port = 0U,
                                         .workspace = workspace,
                                         .maximum_device_count = 16U,
                                         .workspace_limit_bytes = 64ULL * 1024ULL * 1024ULL});
    ASSERT_TRUE(started) << started.error().message;

    const std::string hello = Json{{"requestId", "req-session-1"},
                                   {"machineId", "EDGE-01"},
                                   {"productionLineId", "LINE-01"},
                                   {"softwareVersion", "0.1.0"},
                                   {"supportedProtocolVersions", {1}},
                                   {"capabilities", {"system.requestStatus", "service.restart"}}}
                                  .dump();
    auto session =
        request(QNetworkAccessManager::PostOperation, base_url(runtime, "/api/uplink/v1/sessions"),
                QByteArray::fromStdString(hello));
    ASSERT_EQ(session.status, 201) << session.body.constData();
    const auto session_json = Json::parse(session.body.toStdString());
    ASSERT_EQ(session_json["protocolVersion"], 1U);

    QWebSocket socket;
    bool connected = false;
    QString received_text;
    std::atomic_size_t received_commands{};
    QObject::connect(&socket, &QWebSocket::connected, [&connected] { connected = true; });
    QObject::connect(&socket, &QWebSocket::textMessageReceived,
                     [&received_text, &received_commands](const QString& message) {
                         received_text = message;
                         auto envelope =
                             paperbreak::uplink::parse_message_envelope(message.toStdString());
                         if (envelope && envelope.value().message_type == "command")
                             ++received_commands;
                     });
    socket.open(QUrl(QString::fromStdString(session_json["webSocketUrl"].get<std::string>())));
    ASSERT_TRUE(wait_until([&connected] { return connected; }));

    paperbreak::uplink::MessageEnvelope status{.protocol_version = 1U,
                                               .message_type = "status.update",
                                               .message_id = "msg-status-1",
                                               .machine_id = "EDGE-01",
                                               .sequence = 1U,
                                               .timestamp = "2026-08-05T01:02:03.004Z",
                                               .payload_json = R"({"service":"Running"})"};
    auto status_text = paperbreak::uplink::serialize_message_envelope(status);
    ASSERT_TRUE(status_text);
    socket.sendTextMessage(QString::fromStdString(status_text.value()));
    ASSERT_TRUE(wait_until([&received_text] { return !received_text.isEmpty(); }));
    EXPECT_EQ(paperbreak::uplink::parse_message_envelope(received_text.toStdString())
                  .value()
                  .message_type,
              "ack");

    ASSERT_TRUE(runtime.set_fault_profile(
        "EDGE-01", paperbreak::uplink::simulator::FaultProfile{.replay_commands = true}));
    ASSERT_TRUE(runtime.enqueue_command({.command_id = "command-replay-1",
                                         .machine_id = "EDGE-01",
                                         .command_type = "system.requestStatus",
                                         .deadline = "2026-08-05T02:02:03.004Z"}));
    EXPECT_TRUE(wait_until([&received_commands] { return received_commands.load() == 2U; }));

    paperbreak::uplink::PreviewFrame frame{
        .machine_id = "EDGE-01",
        .camera_id = "CAM01",
        .message_id = "preview-1",
        .sequence = 2U,
        .timestamp = "2026-08-05T01:02:03.100Z",
        .jpeg = {std::byte{0xff}, std::byte{0xd8}, std::byte{0xff}, std::byte{0xd9}}};
    auto preview = paperbreak::uplink::encode_preview_frame(frame);
    ASSERT_TRUE(preview);
    socket.sendBinaryMessage(QByteArray(reinterpret_cast<const char*>(preview.value().data()),
                                        static_cast<qsizetype>(preview.value().size())));
    ASSERT_TRUE(wait_until([&runtime] {
        const auto snapshot = runtime.snapshot();
        return !snapshot.devices.empty() && snapshot.devices.front().received_previews == 1U;
    }));

    const std::string event =
        Json{{"requestId", "req-event-1"}, {"eventId", "EVT-01"}, {"state", "Confirmed"}}.dump();
    auto event_response = request(QNetworkAccessManager::PutOperation,
                                  base_url(runtime, "/api/uplink/v1/devices/EDGE-01/events/EVT-01"),
                                  QByteArray::fromStdString(event));
    ASSERT_EQ(event_response.status, 202) << event_response.body.constData();
    auto duplicate_event =
        request(QNetworkAccessManager::PutOperation,
                base_url(runtime, "/api/uplink/v1/devices/EDGE-01/events/EVT-01"),
                QByteArray::fromStdString(event));
    ASSERT_EQ(duplicate_event.status, 202) << duplicate_event.body.constData();
    const std::string conflicting_event =
        Json{{"requestId", "req-event-1"}, {"eventId", "EVT-01"}, {"state", "Rejected"}}.dump();
    auto event_conflict = request(QNetworkAccessManager::PutOperation,
                                  base_url(runtime, "/api/uplink/v1/devices/EDGE-01/events/EVT-01"),
                                  QByteArray::fromStdString(conflicting_event));
    ASSERT_EQ(event_conflict.status, 409) << event_conflict.body.constData();

    const QByteArray file("hello uplink");
    const std::string file_digest = digest(file);
    const std::string upload_request =
        Json{{"requestId", "req-upload-1"},       {"eventId", "EVT-01"},
             {"logicalFileId", "manifest-1"},     {"fileName", "manifest.json"},
             {"contentType", "application/json"}, {"totalBytes", file.size()},
             {"chunkBytes", file.size()},         {"sha256", file_digest}}
            .dump();
    auto upload = request(QNetworkAccessManager::PostOperation,
                          base_url(runtime, "/api/uplink/v1/devices/EDGE-01/uploads"),
                          QByteArray::fromStdString(upload_request));
    ASSERT_EQ(upload.status, 201) << upload.body.constData();
    const std::string upload_id = Json::parse(upload.body.toStdString())["uploadId"];
    auto invalid_range = request(
        QNetworkAccessManager::PutOperation,
        base_url(runtime, QStringLiteral("/api/uplink/v1/devices/EDGE-01/uploads/%1/chunks/0")
                              .arg(QString::fromStdString(upload_id))),
        file, QByteArray::fromStdString(file_digest), "bytes 1-11/12");
    ASSERT_EQ(invalid_range.status, 400) << invalid_range.body.constData();
    auto chunk = request(
        QNetworkAccessManager::PutOperation,
        base_url(runtime, QStringLiteral("/api/uplink/v1/devices/EDGE-01/uploads/%1/chunks/0")
                              .arg(QString::fromStdString(upload_id))),
        file, QByteArray::fromStdString(file_digest), "bytes 0-11/12");
    ASSERT_EQ(chunk.status, 202) << chunk.body.constData();
    auto duplicate_chunk = request(
        QNetworkAccessManager::PutOperation,
        base_url(runtime, QStringLiteral("/api/uplink/v1/devices/EDGE-01/uploads/%1/chunks/0")
                              .arg(QString::fromStdString(upload_id))),
        file, QByteArray::fromStdString(file_digest), "bytes 0-11/12");
    ASSERT_EQ(duplicate_chunk.status, 202) << duplicate_chunk.body.constData();

    const std::string corrupt_request = Json{
        {"requestId", "req-upload-corrupt"},
        {"eventId", "EVT-01"},
        {"logicalFileId", "corrupt-1"},
        {"fileName", "corrupt.bin"},
        {"contentType", "application/octet-stream"},
        {"totalBytes", file.size()},
        {"chunkBytes", file.size()},
        {"sha256", file_digest}}.dump();
    auto corrupt_upload = request(QNetworkAccessManager::PostOperation,
                                  base_url(runtime, "/api/uplink/v1/devices/EDGE-01/uploads"),
                                  QByteArray::fromStdString(corrupt_request));
    ASSERT_EQ(corrupt_upload.status, 201) << corrupt_upload.body.constData();
    const std::string corrupt_upload_id =
        Json::parse(corrupt_upload.body.toStdString())["uploadId"];
    auto corrupt_chunk = request(
        QNetworkAccessManager::PutOperation,
        base_url(runtime, QStringLiteral("/api/uplink/v1/devices/EDGE-01/uploads/%1/chunks/0")
                              .arg(QString::fromStdString(corrupt_upload_id))),
        file, QByteArray::fromStdString(file_digest), "bytes 0-11/12");
    ASSERT_EQ(corrupt_chunk.status, 202) << corrupt_chunk.body.constData();

    runtime.stop();
    sqlite3* raw_database = nullptr;
    ASSERT_EQ(sqlite3_open16((workspace / "simulator.db").wstring().c_str(), &raw_database),
              SQLITE_OK);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> database(raw_database, sqlite3_close_v2);
    sqlite3_stmt* raw_statement = nullptr;
    ASSERT_EQ(
        sqlite3_prepare_v2(database.get(), "PRAGMA user_version", -1, &raw_statement, nullptr),
        SQLITE_OK);
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw_statement,
                                                                         sqlite3_finalize);
    ASSERT_EQ(sqlite3_step(statement.get()), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(statement.get(), 0), 1);
    statement.reset();
    database.reset();

    QFile corrupted(
        QString::fromStdWString((workspace / ".partial" / corrupt_upload_id).wstring()));
    ASSERT_TRUE(corrupted.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(corrupted.write("tampered"), 8);
    corrupted.close();

    auto restarted = runtime.start(Options{.listen_address = "127.0.0.1",
                                           .port = 0U,
                                           .workspace = workspace,
                                           .maximum_device_count = 16U,
                                           .workspace_limit_bytes = 64ULL * 1024ULL * 1024ULL});
    ASSERT_TRUE(restarted) << restarted.error().message;
    auto status_after_restart =
        request(QNetworkAccessManager::GetOperation,
                base_url(runtime, QStringLiteral("/api/uplink/v1/devices/EDGE-01/uploads/%1")
                                      .arg(QString::fromStdString(upload_id))));
    ASSERT_EQ(status_after_restart.status, 200) << status_after_restart.body.constData();
    EXPECT_EQ(Json::parse(status_after_restart.body.toStdString())["receivedChunks"].size(), 1U);
    auto corrupt_status =
        request(QNetworkAccessManager::GetOperation,
                base_url(runtime, QStringLiteral("/api/uplink/v1/devices/EDGE-01/uploads/%1")
                                      .arg(QString::fromStdString(corrupt_upload_id))));
    ASSERT_EQ(corrupt_status.status, 200) << corrupt_status.body.constData();
    EXPECT_EQ(Json::parse(corrupt_status.body.toStdString())["state"], "Quarantined");
    auto complete = request(
        QNetworkAccessManager::PostOperation,
        base_url(runtime, QStringLiteral("/api/uplink/v1/devices/EDGE-01/uploads/%1/complete")
                              .arg(QString::fromStdString(upload_id))));
    ASSERT_EQ(complete.status, 200) << complete.body.constData();
    runtime.stop();
    EXPECT_TRUE(
        std::filesystem::exists(workspace / "events" / "EDGE-01" / "EVT-01" / "manifest-1"));
    EXPECT_TRUE(
        std::filesystem::exists(workspace / ".quarantine" / (corrupt_upload_id + ".corrupt")));
}

TEST(UplinkSimulator, AcceptsSixteenDevicesAndRejectsTheSeventeenth)
{
    Runtime runtime;
    auto started = runtime.start(Options{.listen_address = "127.0.0.1",
                                         .port = 0U,
                                         .workspace = unique_workspace(),
                                         .maximum_device_count = 16U,
                                         .workspace_limit_bytes = 16ULL * 1024ULL * 1024ULL});
    ASSERT_TRUE(started) << started.error().message;
    for (int index = 1; index <= 17; ++index)
    {
        const std::string machine = "EDGE-" + std::to_string(index);
        const std::string hello = Json{
            {"requestId", "req-" + std::to_string(index)},
            {"machineId", machine},
            {"productionLineId", "LINE-01"},
            {"softwareVersion", "0.1.0"},
            {"supportedProtocolVersions", {1}},
            {"capabilities",
             Json::array()}}.dump();
        const auto response =
            request(QNetworkAccessManager::PostOperation,
                    base_url(runtime, "/api/uplink/v1/sessions"), QByteArray::fromStdString(hello));
        EXPECT_EQ(response.status, index <= 16 ? 201 : 503) << response.body.constData();
    }
    runtime.stop();
}

TEST(UplinkSimulator, EnforcesCapabilityAndBoundedCommandQueue)
{
    Runtime runtime;
    auto started = runtime.start(Options{.listen_address = "127.0.0.1",
                                         .port = 0U,
                                         .workspace = unique_workspace(),
                                         .maximum_device_count = 16U,
                                         .workspace_limit_bytes = 16ULL * 1024ULL * 1024ULL});
    ASSERT_TRUE(started) << started.error().message;
    const std::string hello =
        Json{{"requestId", "req-command-session"}, {"machineId", "EDGE-COMMAND"},
             {"productionLineId", "LINE-01"},      {"softwareVersion", "0.1.0"},
             {"supportedProtocolVersions", {1}},   {"capabilities", {"system.requestStatus"}}}
            .dump();
    ASSERT_EQ(request(QNetworkAccessManager::PostOperation,
                      base_url(runtime, "/api/uplink/v1/sessions"),
                      QByteArray::fromStdString(hello))
                  .status,
              201);
    ASSERT_TRUE(wait_until([&runtime] { return runtime.snapshot().devices.size() == 1U; }));

    auto unsupported = runtime.enqueue_command({.command_id = "unsupported-1",
                                                .machine_id = "EDGE-COMMAND",
                                                .command_type = "service.restart",
                                                .deadline = "2026-08-05T02:02:03.004Z",
                                                .operator_confirmed = true});
    ASSERT_FALSE(unsupported);
    EXPECT_EQ(unsupported.error().business_code, "SYS_NOT_SUPPORTED");

    for (std::size_t index = 0U;
         index < paperbreak::uplink::simulator::command_queue_capacity_per_device; ++index)
        ASSERT_TRUE(runtime.enqueue_command({.command_id = "queued-" + std::to_string(index),
                                             .machine_id = "EDGE-COMMAND",
                                             .command_type = "system.requestStatus",
                                             .deadline = "2026-08-05T02:02:03.004Z"}));
    EXPECT_TRUE(runtime.enqueue_command({.command_id = "queued-0",
                                         .machine_id = "EDGE-COMMAND",
                                         .command_type = "system.requestStatus",
                                         .deadline = "2026-08-05T02:02:03.004Z"}));
    auto conflict = runtime.enqueue_command({.command_id = "queued-0",
                                             .machine_id = "EDGE-COMMAND",
                                             .command_type = "system.requestStatus",
                                             .deadline = "2026-08-05T03:02:03.004Z"});
    ASSERT_FALSE(conflict);
    EXPECT_EQ(conflict.error().business_code, "UPLINK_PROTOCOL_ERROR");
    auto full = runtime.enqueue_command({.command_id = "queued-overflow",
                                         .machine_id = "EDGE-COMMAND",
                                         .command_type = "system.requestStatus",
                                         .deadline = "2026-08-05T02:02:03.004Z"});
    ASSERT_FALSE(full);
    EXPECT_EQ(full.error().business_code, "UPLINK_SERVER_BUSY");
    runtime.stop();
}

TEST(UplinkSimulator, CancelsInjectedDelayDuringBoundedShutdown)
{
    Runtime runtime;
    auto started = runtime.start(Options{.listen_address = "127.0.0.1",
                                         .port = 0U,
                                         .workspace = unique_workspace(),
                                         .maximum_device_count = 16U,
                                         .workspace_limit_bytes = 16ULL * 1024ULL * 1024ULL});
    ASSERT_TRUE(started) << started.error().message;
    ASSERT_TRUE(runtime.set_fault_profile(
        "EDGE-DELAY", paperbreak::uplink::simulator::FaultProfile{.response_delay_ms = 60000U}));
    const QUrl session_url = base_url(runtime, "/api/uplink/v1/sessions");
    const QByteArray hello = QByteArray::fromStdString(Json{
        {"requestId", "req-delay"},
        {"machineId", "EDGE-DELAY"},
        {"productionLineId", "LINE-01"},
        {"softwareVersion", "0.1.0"},
        {"supportedProtocolVersions", {1}},
        {"capabilities", Json::array()}}.dump());
    auto pending = std::async(std::launch::async, [session_url, hello] {
        return request(QNetworkAccessManager::PostOperation, session_url, hello);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    const auto before_stop = std::chrono::steady_clock::now();
    runtime.stop(std::chrono::seconds{1});
    EXPECT_LT(std::chrono::steady_clock::now() - before_stop, std::chrono::seconds{1});
    ASSERT_NE(pending.wait_for(std::chrono::seconds{2}), std::future_status::timeout);
}

} // namespace
