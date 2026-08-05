#include "workspace.hpp"

#include "paperbreak/common/error.hpp"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QFile>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace paperbreak::uplink::simulator
{
namespace
{
using Json = nlohmann::json;

class Statement final
{
  public:
    explicit Statement(sqlite3_stmt* value = nullptr) : value_(value) {}
    ~Statement()
    {
        if (value_ != nullptr)
            sqlite3_finalize(value_);
    }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    Statement(Statement&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    Statement& operator=(Statement&& other) noexcept
    {
        if (this != &other)
        {
            if (value_ != nullptr)
                sqlite3_finalize(value_);
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }
    [[nodiscard]] sqlite3_stmt* get() const noexcept
    {
        return value_;
    }

  private:
    sqlite3_stmt* value_{};
};

Error workspace_error(std::string code, std::string message, std::string operation,
                      const bool retryable = false)
{
    return make_error(std::move(code), Severity::error, std::move(message), "uplink-simulator",
                      std::move(operation), retryable);
}

Result<void> execute(sqlite3* database, const std::string_view sql,
                     const std::string_view operation)
{
    char* native_message = nullptr;
    const int result =
        sqlite3_exec(database, std::string{sql}.c_str(), nullptr, nullptr, &native_message);
    if (result == SQLITE_OK)
        return Result<void>::success();
    auto error = workspace_error("DATABASE_ERROR", "模拟器 SQLite 操作失败", std::string{operation},
                                 result == SQLITE_BUSY || result == SQLITE_LOCKED);
    error.native_domain = "sqlite";
    error.native_code = std::to_string(result);
    if (native_message != nullptr)
    {
        error.details.push_back({.key = "summary", .value = native_message});
        sqlite3_free(native_message);
    }
    return Result<void>::failure(std::move(error));
}

Result<Statement> prepare(sqlite3* database, const std::string_view sql,
                          const std::string_view operation)
{
    sqlite3_stmt* statement = nullptr;
    const int result =
        sqlite3_prepare_v2(database, sql.data(), static_cast<int>(sql.size()), &statement, nullptr);
    if (result == SQLITE_OK)
        return Result<Statement>::success(Statement{statement});
    auto error =
        workspace_error("DATABASE_ERROR", "模拟器 SQLite 语句准备失败", std::string{operation},
                        result == SQLITE_BUSY || result == SQLITE_LOCKED);
    error.native_domain = "sqlite";
    error.native_code = std::to_string(result);
    return Result<Statement>::failure(std::move(error));
}

bool bind_text(sqlite3_stmt* statement, const int index, const std::string_view value)
{
    return sqlite3_bind_text(statement, index, value.data(), static_cast<int>(value.size()),
                             SQLITE_TRANSIENT) == SQLITE_OK;
}

bool bind_i64(sqlite3_stmt* statement, const int index, const std::uint64_t value)
{
    if (value > static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max()))
        return false;
    return sqlite3_bind_int64(statement, index, static_cast<sqlite3_int64>(value)) == SQLITE_OK;
}

std::string column_text(sqlite3_stmt* statement, const int index)
{
    const auto* text = sqlite3_column_text(statement, index);
    return text == nullptr ? std::string{} : reinterpret_cast<const char*>(text);
}

Result<void> step_done(sqlite3_stmt* statement, const std::string_view operation)
{
    const int result = sqlite3_step(statement);
    if (result == SQLITE_DONE)
        return Result<void>::success();
    auto error = workspace_error("DATABASE_ERROR", "模拟器 SQLite 写入失败", std::string{operation},
                                 result == SQLITE_BUSY || result == SQLITE_LOCKED);
    error.native_domain = "sqlite";
    error.native_code = std::to_string(result);
    return Result<void>::failure(std::move(error));
}

std::string sha256(const std::span<const std::byte> bytes)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayView(reinterpret_cast<const char*>(bytes.data()),
                                static_cast<qsizetype>(bytes.size())));
    return hash.result().toHex().toStdString();
}

Result<std::string> sha256_file(const std::filesystem::path& path,
                                const std::stop_token stop_token = {})
{
    QFile file(QString::fromStdWString(path.wstring()));
    if (!file.open(QIODevice::ReadOnly))
        return Result<std::string>::failure(workspace_error(
            "UPLOAD_TRANSFER_FAILED", "无法读取模拟器上传文件", "upload.verify.read", true));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    // The storage worker uses the platform's default thread stack.  Keeping a
    // 1 MiB hashing buffer on that stack overflows on Windows/MSVC, so keep the
    // bounded buffer on the heap instead.
    std::vector<char> buffer(1024U * 1024U);
    while (!file.atEnd())
    {
        if (stop_token.stop_requested())
            return Result<std::string>::failure(
                workspace_error("UPLOAD_TRANSFER_FAILED", "模拟器关闭已取消文件校验",
                                "simulator.upload.verify.cancel", true));
        const qint64 read = file.read(buffer.data(), static_cast<qint64>(buffer.size()));
        if (read < 0)
            return Result<std::string>::failure(workspace_error(
                "UPLOAD_TRANSFER_FAILED", "读取模拟器上传文件失败", "upload.verify.read", true));
        hash.addData(QByteArrayView(buffer.data(), read));
    }
    return Result<std::string>::success(hash.result().toHex().toStdString());
}

Result<std::string> sha256_file_range(const std::filesystem::path& path, const std::uint64_t offset,
                                      const std::uint64_t length)
{
    QFile file(QString::fromStdWString(path.wstring()));
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<qint64>::max()) ||
        !file.open(QIODevice::ReadOnly) || !file.seek(static_cast<qint64>(offset)))
        return Result<std::string>::failure(workspace_error(
            "UPLOAD_TRANSFER_FAILED", "无法读取恢复分块", "simulator.workspace.recover", true));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    std::vector<char> buffer(1024U * 1024U);
    std::uint64_t remaining = length;
    while (remaining > 0U)
    {
        const auto requested =
            static_cast<qint64>(std::min<std::uint64_t>(remaining, buffer.size()));
        const qint64 read = file.read(buffer.data(), requested);
        if (read != requested)
            return Result<std::string>::failure(
                workspace_error("UPLOAD_TRANSFER_FAILED", "恢复分块长度与 checkpoint 不一致",
                                "simulator.workspace.recover", true));
        hash.addData(QByteArrayView(buffer.data(), read));
        remaining -= static_cast<std::uint64_t>(read);
    }
    return Result<std::string>::success(hash.result().toHex().toStdString());
}

bool quarantine_file(const std::filesystem::path& source,
                     const std::filesystem::path& quarantine_root)
{
    std::error_code error;
    if (!std::filesystem::exists(source, error))
        return true;
    auto destination = quarantine_root / (source.filename().wstring() + L".corrupt");
    for (std::uint32_t suffix = 1U; std::filesystem::exists(destination, error) && suffix < 10000U;
         ++suffix)
        destination = quarantine_root /
                      (source.filename().wstring() + L".corrupt." + std::to_wstring(suffix));
    error.clear();
    std::filesystem::rename(source, destination, error);
    return !error;
}

std::uint64_t directory_bytes(const std::filesystem::path& root)
{
    std::uint64_t total = 0U;
    std::error_code error;
    if (!std::filesystem::exists(root, error))
        return 0U;
    for (std::filesystem::recursive_directory_iterator
             iterator(root, std::filesystem::directory_options::skip_permission_denied, error),
         end;
         !error && iterator != end; iterator.increment(error))
    {
        if (!iterator->is_regular_file(error))
            continue;
        const auto size = iterator->file_size(error);
        if (!error && size <= std::numeric_limits<std::uint64_t>::max() - total)
            total += size;
    }
    return total;
}

Result<void> begin(sqlite3* database)
{
    return execute(database, "BEGIN IMMEDIATE", "simulator.database.begin");
}

void rollback(sqlite3* database) noexcept
{
    sqlite3_exec(database, "ROLLBACK", nullptr, nullptr, nullptr);
}

Result<void> commit(sqlite3* database)
{
    return execute(database, "COMMIT", "simulator.database.commit");
}

Result<void> validate_machine_and_id(const std::string_view machine_id, const std::string_view id,
                                     const std::string_view field)
{
    auto machine = validate_identifier(machine_id, "machineId", 64U);
    if (!machine)
        return machine;
    return validate_identifier(id, field, 128U);
}

} // namespace

Workspace::Workspace() = default;
Workspace::~Workspace() = default;

void Workspace::DatabaseDeleter::operator()(sqlite3* database) const noexcept
{
    if (database != nullptr)
        sqlite3_close_v2(database);
}

Result<WorkspaceReport> Workspace::open(std::filesystem::path root, const std::uint64_t limit_bytes,
                                        const std::size_t maximum_device_count,
                                        const std::stop_token stop_token)
{
    close();
    if (root.empty() || limit_bytes == 0U || maximum_device_count == 0U ||
        maximum_device_count > 16U)
        return Result<WorkspaceReport>::failure(workspace_error(
            "SYS_CONFIG_INVALID", "模拟器工作区参数无效", "simulator.workspace.open"));
    std::error_code file_error;
    root = std::filesystem::absolute(root, file_error).lexically_normal();
    if (file_error)
        return Result<WorkspaceReport>::failure(workspace_error(
            "SYS_CONFIG_INVALID", "无法解析模拟器工作区路径", "simulator.workspace.open"));
    for (const auto& child :
         {std::filesystem::path{".partial"}, std::filesystem::path{".quarantine"},
          std::filesystem::path{"events"}})
    {
        std::filesystem::create_directories(root / child, file_error);
        if (file_error)
            return Result<WorkspaceReport>::failure(workspace_error("STORAGE_PATH_UNAVAILABLE",
                                                                    "无法创建模拟器工作区目录",
                                                                    "simulator.workspace.open"));
    }

    sqlite3* raw = nullptr;
    const std::wstring database_path = (root / "simulator.db").wstring();
    const int open_result = sqlite3_open16(database_path.c_str(), &raw);
    if (open_result != SQLITE_OK || raw == nullptr)
    {
        if (raw != nullptr)
            sqlite3_close_v2(raw);
        return Result<WorkspaceReport>::failure(workspace_error(
            "DATABASE_OPEN_FAILED", "无法打开模拟器 SQLite 工作区", "simulator.workspace.open"));
    }
    database_.reset(raw);
    root_ = std::move(root);
    limit_bytes_ = limit_bytes;
    maximum_device_count_ = maximum_device_count;
    stop_token_ = stop_token;

    {
        auto existing_version =
            prepare(database_.get(), "PRAGMA user_version", "simulator.workspace.version");
        if (!existing_version || sqlite3_step(existing_version.value().get()) != SQLITE_ROW)
        {
            close();
            return Result<WorkspaceReport>::failure(workspace_error(
                "DATABASE_ERROR", "无法读取模拟器 schema 版本", "simulator.workspace.version"));
        }
        const int user_version = sqlite3_column_int(existing_version.value().get(), 0);
        if (user_version > 1)
        {
            close();
            return Result<WorkspaceReport>::failure(workspace_error(
                "DATABASE_MIGRATION_FAILED", "模拟器工作区版本高于当前支持的 schema v1",
                "simulator.workspace.version"));
        }
    }

    constexpr std::string_view schema = R"sql(
PRAGMA foreign_keys=ON;
PRAGMA journal_mode=WAL;
PRAGMA synchronous=FULL;
CREATE TABLE IF NOT EXISTS devices(
 machine_id TEXT PRIMARY KEY, production_line_id TEXT NOT NULL, software_version TEXT NOT NULL,
 capabilities_json TEXT NOT NULL, last_seen TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS sessions(
 session_id TEXT PRIMARY KEY, request_id TEXT NOT NULL UNIQUE, machine_id TEXT NOT NULL,
 response_json TEXT NOT NULL, connected_at TEXT NOT NULL, disconnected_at TEXT,
 FOREIGN KEY(machine_id) REFERENCES devices(machine_id));
CREATE TABLE IF NOT EXISTS messages(
 message_id TEXT PRIMARY KEY, machine_id TEXT NOT NULL, message_type TEXT NOT NULL,
 sequence INTEGER NOT NULL, timestamp TEXT NOT NULL, payload_json TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS events(
 machine_id TEXT NOT NULL, event_id TEXT NOT NULL, request_id TEXT NOT NULL UNIQUE,
 body_sha256 TEXT NOT NULL, metadata_json TEXT NOT NULL, received_at TEXT NOT NULL,
 PRIMARY KEY(machine_id,event_id));
CREATE TABLE IF NOT EXISTS uploads(
 upload_id TEXT PRIMARY KEY, request_id TEXT NOT NULL UNIQUE, machine_id TEXT NOT NULL,
 event_id TEXT NOT NULL, logical_file_id TEXT NOT NULL, file_name TEXT NOT NULL,
 content_type TEXT NOT NULL, total_bytes INTEGER NOT NULL, chunk_bytes INTEGER NOT NULL,
 sha256 TEXT NOT NULL, state TEXT NOT NULL, created_at TEXT NOT NULL, completed_at TEXT,
 UNIQUE(machine_id,event_id,logical_file_id));
CREATE TABLE IF NOT EXISTS chunks(
 upload_id TEXT NOT NULL, chunk_index INTEGER NOT NULL, offset_bytes INTEGER NOT NULL,
 length_bytes INTEGER NOT NULL, sha256 TEXT NOT NULL,
 PRIMARY KEY(upload_id,chunk_index), FOREIGN KEY(upload_id) REFERENCES uploads(upload_id));
CREATE TABLE IF NOT EXISTS commands(
 command_id TEXT PRIMARY KEY, machine_id TEXT NOT NULL, command_type TEXT NOT NULL,
 payload_json TEXT NOT NULL, deadline TEXT NOT NULL, state TEXT NOT NULL,
 result_json TEXT, created_at TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS alarms(
 message_id TEXT PRIMARY KEY, machine_id TEXT NOT NULL, payload_json TEXT NOT NULL,
 timestamp TEXT NOT NULL);
PRAGMA user_version=1;
)sql";
    auto initialized = execute(database_.get(), schema, "simulator.workspace.schema");
    if (!initialized)
    {
        close();
        return Result<WorkspaceReport>::failure(initialized.error());
    }
    std::size_t recovered_uploads = 0U;
    std::size_t quarantined_uploads = 0U;
    std::unordered_set<std::string> active_ids;
    auto active = prepare(database_.get(), R"sql(
SELECT upload_id,total_bytes FROM uploads WHERE state='Uploading')sql",
                          "simulator.workspace.recover.uploads");
    if (!active)
    {
        close();
        return Result<WorkspaceReport>::failure(active.error());
    }
    while (sqlite3_step(active.value().get()) == SQLITE_ROW)
    {
        const std::string upload_id = column_text(active.value().get(), 0);
        const auto total_bytes =
            static_cast<std::uint64_t>(sqlite3_column_int64(active.value().get(), 1));
        active_ids.insert(upload_id);
        const auto partial_path = root_ / ".partial" / upload_id;
        std::error_code recovery_error;
        const auto file_size = std::filesystem::file_size(partial_path, recovery_error);
        bool valid = !recovery_error && file_size <= total_bytes;
        auto chunks = prepare(database_.get(), R"sql(
SELECT offset_bytes,length_bytes,sha256 FROM chunks WHERE upload_id=?1 ORDER BY chunk_index)sql",
                              "simulator.workspace.recover.chunks");
        if (!chunks || !bind_text(chunks.value().get(), 1, upload_id))
            valid = false;
        while (valid && sqlite3_step(chunks.value().get()) == SQLITE_ROW)
        {
            const auto offset =
                static_cast<std::uint64_t>(sqlite3_column_int64(chunks.value().get(), 0));
            const auto length =
                static_cast<std::uint64_t>(sqlite3_column_int64(chunks.value().get(), 1));
            if (offset > file_size || length > file_size - offset)
            {
                valid = false;
                break;
            }
            auto digest = sha256_file_range(partial_path, offset, length);
            valid = digest && digest.value() == column_text(chunks.value().get(), 2);
        }
        if (valid)
        {
            ++recovered_uploads;
            continue;
        }
        static_cast<void>(quarantine_file(partial_path, root_ / ".quarantine"));
        auto update =
            prepare(database_.get(), "UPDATE uploads SET state='Quarantined' WHERE upload_id=?1",
                    "simulator.workspace.recover.quarantine");
        if (update && bind_text(update.value().get(), 1, upload_id))
            static_cast<void>(
                step_done(update.value().get(), "simulator.workspace.recover.quarantine"));
        ++quarantined_uploads;
    }
    std::error_code iterator_error;
    for (std::filesystem::directory_iterator iterator(root_ / ".partial", iterator_error), end;
         !iterator_error && iterator != end; iterator.increment(iterator_error))
    {
        if (!iterator->is_regular_file(iterator_error))
            continue;
        const std::string name = iterator->path().filename().string();
        if (!active_ids.contains(name))
        {
            if (quarantine_file(iterator->path(), root_ / ".quarantine"))
                ++quarantined_uploads;
        }
    }

    used_bytes_ = directory_bytes(root_ / ".partial") + directory_bytes(root_ / "events") +
                  directory_bytes(root_ / ".quarantine");

    std::size_t devices = 0U;
    auto count =
        prepare(database_.get(), "SELECT COUNT(*) FROM devices", "simulator.devices.count");
    if (count && sqlite3_step(count.value().get()) == SQLITE_ROW)
        devices = static_cast<std::size_t>(sqlite3_column_int64(count.value().get(), 0));
    return Result<WorkspaceReport>::success({.used_bytes = used_bytes_,
                                             .device_count = devices,
                                             .recovered_uploads = recovered_uploads,
                                             .quarantined_uploads = quarantined_uploads});
}

void Workspace::close() noexcept
{
    if (database_ != nullptr)
    {
        int log_frames = 0;
        int checkpointed_frames = 0;
        static_cast<void>(sqlite3_wal_checkpoint_v2(
            database_.get(), nullptr, SQLITE_CHECKPOINT_FULL, &log_frames, &checkpointed_frames));
    }
    database_.reset();
    root_.clear();
    limit_bytes_ = 0U;
    used_bytes_ = 0U;
    maximum_device_count_ = 0U;
    stop_token_ = {};
}

std::uint64_t Workspace::used_bytes() const noexcept
{
    return used_bytes_;
}

Result<StoredSession> Workspace::create_session(const SessionHello& hello, std::string session_id,
                                                std::string server_time, std::string websocket_host,
                                                const std::uint16_t port)
{
    if (database_ == nullptr)
        return Result<StoredSession>::failure(workspace_error(
            "DATABASE_NOT_READY", "模拟器工作区尚未打开", "simulator.session.create"));
    auto duplicate =
        prepare(database_.get(),
                "SELECT machine_id,session_id,response_json FROM sessions WHERE request_id=?1",
                "simulator.session.idempotency");
    if (!duplicate || !bind_text(duplicate.value().get(), 1, hello.request_id))
        return Result<StoredSession>::failure(
            duplicate ? workspace_error("DATABASE_ERROR", "无法绑定会话幂等查询",
                                        "simulator.session.idempotency")
                      : duplicate.error());
    if (sqlite3_step(duplicate.value().get()) == SQLITE_ROW)
    {
        if (column_text(duplicate.value().get(), 0) != hello.machine_id)
            return Result<StoredSession>::failure(workspace_error("UPLINK_PROTOCOL_ERROR",
                                                                  "requestId 已被另一设备使用",
                                                                  "simulator.session.idempotency"));
        return Result<StoredSession>::success(
            {.session_id = column_text(duplicate.value().get(), 1),
             .machine_id = hello.machine_id,
             .response_json = column_text(duplicate.value().get(), 2),
             .duplicate = true});
    }

    auto existing_device = prepare(database_.get(), "SELECT 1 FROM devices WHERE machine_id=?1",
                                   "simulator.device.exists");
    if (!existing_device || !bind_text(existing_device.value().get(), 1, hello.machine_id))
        return Result<StoredSession>::failure(
            existing_device
                ? workspace_error("DATABASE_ERROR", "无法绑定设备查询", "simulator.device.exists")
                : existing_device.error());
    const bool exists = sqlite3_step(existing_device.value().get()) == SQLITE_ROW;
    if (!exists)
    {
        auto count =
            prepare(database_.get(), "SELECT COUNT(*) FROM devices", "simulator.devices.count");
        if (!count || sqlite3_step(count.value().get()) != SQLITE_ROW)
            return Result<StoredSession>::failure(count ? workspace_error("DATABASE_ERROR",
                                                                          "无法读取设备数量",
                                                                          "simulator.devices.count")
                                                        : count.error());
        if (static_cast<std::size_t>(sqlite3_column_int64(count.value().get(), 0)) >=
            maximum_device_count_)
            return Result<StoredSession>::failure(
                workspace_error("UPLINK_SERVER_BUSY", "模拟器已达到设备数量上限",
                                "simulator.session.create", true));
    }

    const std::string capabilities = Json(hello.capabilities).dump();
    const std::string response =
        Json{{"protocolVersion", protocol_version},
             {"requestId", hello.request_id},
             {"sessionId", session_id},
             {"machineId", hello.machine_id},
             {"serverTime", server_time},
             {"heartbeatSeconds", 5U},
             {"webSocketUrl", "ws://" + std::move(websocket_host) + ":" + std::to_string(port) +
                                  "/api/uplink/v1/sessions/" + session_id + "/stream"}}
            .dump();
    auto transaction = begin(database_.get());
    if (!transaction)
        return Result<StoredSession>::failure(transaction.error());
    auto device = prepare(database_.get(), R"sql(
INSERT INTO devices(machine_id,production_line_id,software_version,capabilities_json,last_seen)
VALUES(?1,?2,?3,?4,?5)
ON CONFLICT(machine_id) DO UPDATE SET production_line_id=excluded.production_line_id,
 software_version=excluded.software_version,capabilities_json=excluded.capabilities_json,
 last_seen=excluded.last_seen)sql",
                          "simulator.device.upsert");
    if (!device || !bind_text(device.value().get(), 1, hello.machine_id) ||
        !bind_text(device.value().get(), 2, hello.production_line_id) ||
        !bind_text(device.value().get(), 3, hello.software_version) ||
        !bind_text(device.value().get(), 4, capabilities) ||
        !bind_text(device.value().get(), 5, server_time) ||
        !step_done(device.value().get(), "simulator.device.upsert"))
    {
        rollback(database_.get());
        return Result<StoredSession>::failure(
            workspace_error("DATABASE_ERROR", "无法保存模拟器设备", "simulator.device.upsert"));
    }
    auto session = prepare(database_.get(), R"sql(
INSERT INTO sessions(session_id,request_id,machine_id,response_json,connected_at)
VALUES(?1,?2,?3,?4,?5))sql",
                           "simulator.session.insert");
    if (!session || !bind_text(session.value().get(), 1, session_id) ||
        !bind_text(session.value().get(), 2, hello.request_id) ||
        !bind_text(session.value().get(), 3, hello.machine_id) ||
        !bind_text(session.value().get(), 4, response) ||
        !bind_text(session.value().get(), 5, server_time) ||
        !step_done(session.value().get(), "simulator.session.insert"))
    {
        rollback(database_.get());
        return Result<StoredSession>::failure(
            workspace_error("DATABASE_ERROR", "无法保存模拟器会话", "simulator.session.insert"));
    }
    auto committed = commit(database_.get());
    if (!committed)
        return Result<StoredSession>::failure(committed.error());
    return Result<StoredSession>::success({.session_id = std::move(session_id),
                                           .machine_id = hello.machine_id,
                                           .response_json = response});
}

Result<bool> Workspace::has_session(const std::string_view session_id,
                                    const std::string_view machine_id)
{
    auto statement =
        prepare(database_.get(), "SELECT 1 FROM sessions WHERE session_id=?1 AND machine_id=?2",
                "simulator.session.lookup");
    if (!statement || !bind_text(statement.value().get(), 1, session_id) ||
        !bind_text(statement.value().get(), 2, machine_id))
        return Result<bool>::failure(statement
                                         ? workspace_error("DATABASE_ERROR", "无法查询模拟器会话",
                                                           "simulator.session.lookup")
                                         : statement.error());
    return Result<bool>::success(sqlite3_step(statement.value().get()) == SQLITE_ROW);
}

Result<std::string> Workspace::session_machine(const std::string_view session_id)
{
    auto statement = prepare(database_.get(), "SELECT machine_id FROM sessions WHERE session_id=?1",
                             "simulator.session.machine");
    if (!statement || !bind_text(statement.value().get(), 1, session_id))
        return Result<std::string>::failure(statement ? workspace_error("DATABASE_ERROR",
                                                                        "无法绑定会话设备查询",
                                                                        "simulator.session.machine")
                                                      : statement.error());
    if (sqlite3_step(statement.value().get()) != SQLITE_ROW)
        return Result<std::string>::failure(workspace_error(
            "UPLINK_PROTOCOL_ERROR", "WebSocket 会话不存在", "simulator.session.machine"));
    return Result<std::string>::success(column_text(statement.value().get(), 0));
}

Result<void> Workspace::close_session(const std::string_view session_id,
                                      const std::string_view timestamp)
{
    auto statement =
        prepare(database_.get(), "UPDATE sessions SET disconnected_at=?2 WHERE session_id=?1",
                "simulator.session.close");
    if (!statement || !bind_text(statement.value().get(), 1, session_id) ||
        !bind_text(statement.value().get(), 2, timestamp))
        return Result<void>::failure(
            statement
                ? workspace_error("DATABASE_ERROR", "无法绑定会话关闭", "simulator.session.close")
                : statement.error());
    return step_done(statement.value().get(), "simulator.session.close");
}

Result<void> Workspace::store_message(const MessageEnvelope& envelope)
{
    auto existing = prepare(
        database_.get(),
        "SELECT machine_id,message_type,sequence,payload_json FROM messages WHERE message_id=?1",
        "simulator.message.idempotency");
    if (!existing || !bind_text(existing.value().get(), 1, envelope.message_id))
        return Result<void>::failure(existing
                                         ? workspace_error("DATABASE_ERROR", "无法绑定消息幂等查询",
                                                           "simulator.message.idempotency")
                                         : existing.error());
    if (sqlite3_step(existing.value().get()) == SQLITE_ROW)
    {
        const bool same =
            column_text(existing.value().get(), 0) == envelope.machine_id &&
            column_text(existing.value().get(), 1) == envelope.message_type &&
            static_cast<std::uint64_t>(sqlite3_column_int64(existing.value().get(), 2)) ==
                envelope.sequence &&
            column_text(existing.value().get(), 3) == envelope.payload_json;
        return same ? Result<void>::success()
                    : Result<void>::failure(workspace_error("UPLINK_PROTOCOL_ERROR",
                                                            "messageId 对应内容发生冲突",
                                                            "simulator.message.idempotency"));
    }
    auto statement = prepare(database_.get(), R"sql(
INSERT INTO messages(message_id,machine_id,message_type,sequence,timestamp,payload_json)
VALUES(?1,?2,?3,?4,?5,?6))sql",
                             "simulator.message.insert");
    if (!statement || !bind_text(statement.value().get(), 1, envelope.message_id) ||
        !bind_text(statement.value().get(), 2, envelope.machine_id) ||
        !bind_text(statement.value().get(), 3, envelope.message_type) ||
        !bind_i64(statement.value().get(), 4, envelope.sequence) ||
        !bind_text(statement.value().get(), 5, envelope.timestamp) ||
        !bind_text(statement.value().get(), 6, envelope.payload_json))
        return Result<void>::failure(
            statement
                ? workspace_error("DATABASE_ERROR", "无法绑定消息写入", "simulator.message.insert")
                : statement.error());
    auto inserted = step_done(statement.value().get(), "simulator.message.insert");
    if (!inserted || envelope.message_type != "alarm")
        return inserted;
    auto alarm = prepare(database_.get(), R"sql(
INSERT INTO alarms(message_id,machine_id,payload_json,timestamp) VALUES(?1,?2,?3,?4))sql",
                         "simulator.alarm.insert");
    if (!alarm || !bind_text(alarm.value().get(), 1, envelope.message_id) ||
        !bind_text(alarm.value().get(), 2, envelope.machine_id) ||
        !bind_text(alarm.value().get(), 3, envelope.payload_json) ||
        !bind_text(alarm.value().get(), 4, envelope.timestamp))
        return Result<void>::failure(
            alarm ? workspace_error("DATABASE_ERROR", "无法绑定报警写入", "simulator.alarm.insert")
                  : alarm.error());
    return step_done(alarm.value().get(), "simulator.alarm.insert");
}

Result<std::string> Workspace::store_event(const std::string_view machine_id,
                                           const std::string_view event_id,
                                           const std::string_view request_json)
{
    auto valid = validate_machine_and_id(machine_id, event_id, "eventId");
    if (!valid)
        return Result<std::string>::failure(valid.error());
    if (request_json.empty() || request_json.size() > maximum_json_message_bytes)
        return Result<std::string>::failure(workspace_error(
            "UPLINK_PROTOCOL_ERROR", "事件元数据为空或超过 1 MiB", "simulator.event.store"));
    auto body = Json::parse(request_json, nullptr, false);
    if (body.is_discarded() || !body.is_object() || !body.contains("requestId") ||
        !body["requestId"].is_string())
        return Result<std::string>::failure(workspace_error(
            "UPLINK_PROTOCOL_ERROR", "事件元数据缺少 requestId", "simulator.event.store"));
    const auto request_id = body["requestId"].get<std::string>();
    auto request_valid = validate_identifier(request_id, "requestId", 128U);
    if (!request_valid)
        return Result<std::string>::failure(request_valid.error());
    const auto* data = reinterpret_cast<const std::byte*>(request_json.data());
    const std::string digest = sha256({data, request_json.size()});
    auto existing = prepare(database_.get(),
                            "SELECT body_sha256 FROM events WHERE machine_id=?1 AND event_id=?2",
                            "simulator.event.idempotency");
    if (!existing || !bind_text(existing.value().get(), 1, machine_id) ||
        !bind_text(existing.value().get(), 2, event_id))
        return Result<std::string>::failure(
            existing ? workspace_error("DATABASE_ERROR", "无法查询事件幂等状态",
                                       "simulator.event.idempotency")
                     : existing.error());
    if (sqlite3_step(existing.value().get()) == SQLITE_ROW)
    {
        if (column_text(existing.value().get(), 0) != digest)
            return Result<std::string>::failure(workspace_error("UPLINK_PROTOCOL_ERROR",
                                                                "同一事件 ID 的元数据发生冲突",
                                                                "simulator.event.idempotency"));
        return Result<std::string>::success(
            Json{{"accepted", true}, {"duplicate", true}, {"eventId", event_id}}.dump());
    }
    auto statement = prepare(database_.get(), R"sql(
INSERT INTO events(machine_id,event_id,request_id,body_sha256,metadata_json,received_at)
VALUES(?1,?2,?3,?4,?5,?6))sql",
                             "simulator.event.insert");
    const std::string now = current_utc_timestamp();
    if (!statement || !bind_text(statement.value().get(), 1, machine_id) ||
        !bind_text(statement.value().get(), 2, event_id) ||
        !bind_text(statement.value().get(), 3, request_id) ||
        !bind_text(statement.value().get(), 4, digest) ||
        !bind_text(statement.value().get(), 5, request_json) ||
        !bind_text(statement.value().get(), 6, now))
        return Result<std::string>::failure(
            statement
                ? workspace_error("DATABASE_ERROR", "无法绑定事件写入", "simulator.event.insert")
                : statement.error());
    auto inserted = step_done(statement.value().get(), "simulator.event.insert");
    if (!inserted)
        return Result<std::string>::failure(inserted.error());
    return Result<std::string>::success(
        Json{{"accepted", true}, {"duplicate", false}, {"eventId", event_id}}.dump());
}

Result<StoredUpload> Workspace::create_upload(const std::string_view machine_id,
                                              const UploadCreateRequest& request,
                                              std::string upload_id)
{
    auto valid = validate_machine_and_id(machine_id, request.event_id, "eventId");
    if (!valid)
        return Result<StoredUpload>::failure(valid.error());
    auto existing = prepare(database_.get(), R"sql(
SELECT upload_id,total_bytes,chunk_bytes,sha256,file_name,content_type,state
FROM uploads WHERE machine_id=?1 AND event_id=?2 AND logical_file_id=?3)sql",
                            "simulator.upload.idempotency");
    if (!existing || !bind_text(existing.value().get(), 1, machine_id) ||
        !bind_text(existing.value().get(), 2, request.event_id) ||
        !bind_text(existing.value().get(), 3, request.logical_file_id))
        return Result<StoredUpload>::failure(
            existing ? workspace_error("DATABASE_ERROR", "无法查询上传幂等状态",
                                       "simulator.upload.idempotency")
                     : existing.error());
    if (sqlite3_step(existing.value().get()) == SQLITE_ROW)
    {
        const bool same =
            static_cast<std::uint64_t>(sqlite3_column_int64(existing.value().get(), 1)) ==
                request.total_bytes &&
            static_cast<std::uint32_t>(sqlite3_column_int64(existing.value().get(), 2)) ==
                request.chunk_bytes &&
            column_text(existing.value().get(), 3) == request.sha256 &&
            column_text(existing.value().get(), 4) == request.file_name &&
            column_text(existing.value().get(), 5) == request.content_type;
        if (!same)
            return Result<StoredUpload>::failure(workspace_error("UPLOAD_REJECTED",
                                                                 "同一逻辑文件的上传描述发生冲突",
                                                                 "simulator.upload.idempotency"));
        const std::string existing_id = column_text(existing.value().get(), 0);
        return Result<StoredUpload>::success(
            {.upload_id = existing_id,
             .response_json = Json{{"uploadId", existing_id},
                                   {"duplicate", true},
                                   {"state", column_text(existing.value().get(), 6)}}
                                  .dump(),
             .duplicate = true});
    }
    const auto partial_path = root_ / ".partial" / upload_id;
    QFile partial(QString::fromStdWString(partial_path.wstring()));
    if (!partial.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return Result<StoredUpload>::failure(workspace_error(
            "UPLOAD_TRANSFER_FAILED", "无法创建上传临时文件", "simulator.upload.partial", true));
    partial.close();

    auto statement = prepare(database_.get(), R"sql(
INSERT INTO uploads(upload_id,request_id,machine_id,event_id,logical_file_id,file_name,
 content_type,total_bytes,chunk_bytes,sha256,state,created_at)
VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,'Uploading',?11))sql",
                             "simulator.upload.insert");
    const std::string now = current_utc_timestamp();
    if (!statement || !bind_text(statement.value().get(), 1, upload_id) ||
        !bind_text(statement.value().get(), 2, request.request_id) ||
        !bind_text(statement.value().get(), 3, machine_id) ||
        !bind_text(statement.value().get(), 4, request.event_id) ||
        !bind_text(statement.value().get(), 5, request.logical_file_id) ||
        !bind_text(statement.value().get(), 6, request.file_name) ||
        !bind_text(statement.value().get(), 7, request.content_type) ||
        !bind_i64(statement.value().get(), 8, request.total_bytes) ||
        !bind_i64(statement.value().get(), 9, request.chunk_bytes) ||
        !bind_text(statement.value().get(), 10, request.sha256) ||
        !bind_text(statement.value().get(), 11, now))
    {
        std::error_code cleanup_error;
        std::filesystem::remove(partial_path, cleanup_error);
        return Result<StoredUpload>::failure(
            statement
                ? workspace_error("DATABASE_ERROR", "无法绑定上传写入", "simulator.upload.insert")
                : statement.error());
    }
    auto inserted = step_done(statement.value().get(), "simulator.upload.insert");
    if (!inserted)
    {
        std::error_code cleanup_error;
        std::filesystem::remove(partial_path, cleanup_error);
        return Result<StoredUpload>::failure(inserted.error());
    }
    return Result<StoredUpload>::success(
        {.upload_id = upload_id,
         .response_json =
             Json{{"uploadId", upload_id}, {"duplicate", false}, {"state", "Uploading"}}.dump()});
}

Result<std::string> Workspace::upload_status(const std::string_view machine_id,
                                             const std::string_view upload_id)
{
    auto statement = prepare(database_.get(), R"sql(
SELECT state,total_bytes,chunk_bytes,sha256 FROM uploads WHERE upload_id=?1 AND machine_id=?2)sql",
                             "simulator.upload.status");
    if (!statement || !bind_text(statement.value().get(), 1, upload_id) ||
        !bind_text(statement.value().get(), 2, machine_id))
        return Result<std::string>::failure(
            statement
                ? workspace_error("DATABASE_ERROR", "无法查询上传状态", "simulator.upload.status")
                : statement.error());
    if (sqlite3_step(statement.value().get()) != SQLITE_ROW)
        return Result<std::string>::failure(
            workspace_error("UPLOAD_REJECTED", "上传任务不存在", "simulator.upload.status"));
    Json chunks = Json::array();
    auto chunk_statement = prepare(
        database_.get(), "SELECT chunk_index FROM chunks WHERE upload_id=?1 ORDER BY chunk_index",
        "simulator.upload.chunks");
    if (!chunk_statement || !bind_text(chunk_statement.value().get(), 1, upload_id))
        return Result<std::string>::failure(
            chunk_statement
                ? workspace_error("DATABASE_ERROR", "无法查询上传分块", "simulator.upload.chunks")
                : chunk_statement.error());
    while (sqlite3_step(chunk_statement.value().get()) == SQLITE_ROW)
        chunks.push_back(sqlite3_column_int64(chunk_statement.value().get(), 0));
    return Result<std::string>::success(Json{
        {"uploadId", upload_id},
        {"state", column_text(statement.value().get(), 0)},
        {"totalBytes", sqlite3_column_int64(statement.value().get(), 1)},
        {"chunkBytes", sqlite3_column_int64(statement.value().get(), 2)},
        {"sha256", column_text(statement.value().get(), 3)},
        {"receivedChunks",
         std::move(chunks)}}.dump());
}

Result<std::size_t> Workspace::active_upload_count()
{
    auto count = prepare(database_.get(), "SELECT COUNT(*) FROM uploads WHERE state='Uploading'",
                         "simulator.upload.active-count");
    if (!count || sqlite3_step(count.value().get()) != SQLITE_ROW)
        return Result<std::size_t>::failure(count ? workspace_error("DATABASE_ERROR",
                                                                    "无法读取活动上传数量",
                                                                    "simulator.upload.active-count")
                                                  : count.error());
    return Result<std::size_t>::success(
        static_cast<std::size_t>(sqlite3_column_int64(count.value().get(), 0)));
}

Result<std::vector<StoredUploadSnapshot>> Workspace::upload_snapshots()
{
    auto statement = prepare(database_.get(), R"sql(
SELECT u.upload_id,u.machine_id,u.event_id,u.logical_file_id,u.state,u.total_bytes,
       COALESCE(SUM(c.length_bytes),0)
FROM uploads u LEFT JOIN chunks c ON c.upload_id=u.upload_id
GROUP BY u.upload_id,u.machine_id,u.event_id,u.logical_file_id,u.state,u.total_bytes
ORDER BY u.created_at DESC LIMIT 256)sql",
                             "simulator.upload.snapshots");
    if (!statement)
        return Result<std::vector<StoredUploadSnapshot>>::failure(statement.error());
    std::vector<StoredUploadSnapshot> snapshots;
    while (sqlite3_step(statement.value().get()) == SQLITE_ROW)
        snapshots.push_back({.upload_id = column_text(statement.value().get(), 0),
                             .machine_id = column_text(statement.value().get(), 1),
                             .event_id = column_text(statement.value().get(), 2),
                             .logical_file_id = column_text(statement.value().get(), 3),
                             .state = column_text(statement.value().get(), 4),
                             .received_bytes = static_cast<std::uint64_t>(
                                 sqlite3_column_int64(statement.value().get(), 6)),
                             .total_bytes = static_cast<std::uint64_t>(
                                 sqlite3_column_int64(statement.value().get(), 5))});
    return Result<std::vector<StoredUploadSnapshot>>::success(std::move(snapshots));
}

Result<std::string> Workspace::store_chunk(const std::string_view machine_id,
                                           const std::string_view upload_id,
                                           const std::uint32_t chunk_index,
                                           const std::string_view expected_sha256,
                                           const std::string_view content_range,
                                           const std::span<const std::byte> bytes,
                                           const bool force_mismatch)
{
    if (!is_sha256_hex(expected_sha256) || bytes.empty() || bytes.size() > maximum_chunk_bytes)
        return Result<std::string>::failure(workspace_error(
            "UPLINK_PROTOCOL_ERROR", "上传分块摘要或长度无效", "simulator.upload.chunk"));
    auto upload = prepare(database_.get(), R"sql(
SELECT total_bytes,chunk_bytes,state FROM uploads WHERE upload_id=?1 AND machine_id=?2)sql",
                          "simulator.upload.chunk.lookup");
    if (!upload || !bind_text(upload.value().get(), 1, upload_id) ||
        !bind_text(upload.value().get(), 2, machine_id))
        return Result<std::string>::failure(
            upload ? workspace_error("DATABASE_ERROR", "无法查询分块上传",
                                     "simulator.upload.chunk.lookup")
                   : upload.error());
    if (sqlite3_step(upload.value().get()) != SQLITE_ROW)
        return Result<std::string>::failure(
            workspace_error("UPLOAD_REJECTED", "上传任务不存在", "simulator.upload.chunk"));
    const auto total = static_cast<std::uint64_t>(sqlite3_column_int64(upload.value().get(), 0));
    const auto chunk_size =
        static_cast<std::uint64_t>(sqlite3_column_int64(upload.value().get(), 1));
    const std::string state = column_text(upload.value().get(), 2);
    if (state == "Completed")
        return Result<std::string>::success(
            Json{{"uploadId", upload_id}, {"chunkIndex", chunk_index}, {"duplicate", true}}.dump());
    if (chunk_size == 0U || chunk_index > std::numeric_limits<std::uint64_t>::max() / chunk_size)
        return Result<std::string>::failure(
            workspace_error("UPLOAD_REJECTED", "分块索引溢出", "simulator.upload.chunk"));
    const std::uint64_t offset = static_cast<std::uint64_t>(chunk_index) * chunk_size;
    if (offset >= total)
        return Result<std::string>::failure(
            workspace_error("UPLOAD_REJECTED", "分块索引超过文件范围", "simulator.upload.chunk"));
    const std::uint64_t required = std::min(chunk_size, total - offset);
    const std::string expected_range = "bytes " + std::to_string(offset) + "-" +
                                       std::to_string(offset + required - 1U) + "/" +
                                       std::to_string(total);
    if (content_range != expected_range)
        return Result<std::string>::failure(
            workspace_error("UPLINK_PROTOCOL_ERROR", "Content-Range 与分块索引或上传描述不一致",
                            "simulator.upload.chunk.range"));
    if (bytes.size() != required)
        return Result<std::string>::failure(workspace_error(
            "UPLOAD_REJECTED", "分块长度与文件范围不一致", "simulator.upload.chunk"));
    const std::string actual = sha256(bytes);
    if (force_mismatch || actual != expected_sha256)
        return Result<std::string>::failure(workspace_error(
            "UPLOAD_CHECKSUM_MISMATCH", "上传分块 SHA-256 不匹配", "simulator.upload.chunk", true));

    auto existing =
        prepare(database_.get(),
                "SELECT length_bytes,sha256 FROM chunks WHERE upload_id=?1 AND chunk_index=?2",
                "simulator.upload.chunk.idempotency");
    if (!existing || !bind_text(existing.value().get(), 1, upload_id) ||
        !bind_i64(existing.value().get(), 2, chunk_index))
        return Result<std::string>::failure(
            existing ? workspace_error("DATABASE_ERROR", "无法查询分块幂等状态",
                                       "simulator.upload.chunk.idempotency")
                     : existing.error());
    if (sqlite3_step(existing.value().get()) == SQLITE_ROW)
    {
        if (static_cast<std::uint64_t>(sqlite3_column_int64(existing.value().get(), 0)) !=
                bytes.size() ||
            column_text(existing.value().get(), 1) != actual)
            return Result<std::string>::failure(workspace_error(
                "UPLOAD_REJECTED", "重复分块内容发生冲突", "simulator.upload.chunk.idempotency"));
        return Result<std::string>::success(
            Json{{"uploadId", upload_id}, {"chunkIndex", chunk_index}, {"duplicate", true}}.dump());
    }
    if (bytes.size() > limit_bytes_ - std::min(limit_bytes_, used_bytes_))
        return Result<std::string>::failure(
            workspace_error("UPLOAD_TRANSFER_FAILED", "模拟器工作区已达到容量上限",
                            "simulator.upload.chunk.capacity", true));
    QFile partial(QString::fromStdWString((root_ / ".partial" / std::string{upload_id}).wstring()));
    if (!partial.open(QIODevice::ReadWrite) || !partial.seek(static_cast<qint64>(offset)) ||
        partial.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<qint64>(bytes.size())) != static_cast<qint64>(bytes.size()) ||
        !partial.flush())
        return Result<std::string>::failure(workspace_error(
            "UPLOAD_TRANSFER_FAILED", "写入上传分块失败", "simulator.upload.chunk.write", true));
    auto statement = prepare(database_.get(), R"sql(
INSERT INTO chunks(upload_id,chunk_index,offset_bytes,length_bytes,sha256)
VALUES(?1,?2,?3,?4,?5))sql",
                             "simulator.upload.chunk.insert");
    if (!statement || !bind_text(statement.value().get(), 1, upload_id) ||
        !bind_i64(statement.value().get(), 2, chunk_index) ||
        !bind_i64(statement.value().get(), 3, offset) ||
        !bind_i64(statement.value().get(), 4, bytes.size()) ||
        !bind_text(statement.value().get(), 5, actual))
        return Result<std::string>::failure(
            statement ? workspace_error("DATABASE_ERROR", "无法绑定分块记录",
                                        "simulator.upload.chunk.insert")
                      : statement.error());
    auto inserted = step_done(statement.value().get(), "simulator.upload.chunk.insert");
    if (!inserted)
        return Result<std::string>::failure(inserted.error());
    used_bytes_ += bytes.size();
    return Result<std::string>::success(Json{
        {"uploadId", upload_id},
        {"chunkIndex", chunk_index},
        {"duplicate", false},
        {"sha256", actual}}.dump());
}

Result<std::string> Workspace::complete_upload(const std::string_view machine_id,
                                               const std::string_view upload_id,
                                               const bool force_mismatch)
{
    auto upload = prepare(database_.get(), R"sql(
SELECT event_id,logical_file_id,total_bytes,chunk_bytes,sha256,state
FROM uploads WHERE upload_id=?1 AND machine_id=?2)sql",
                          "simulator.upload.complete.lookup");
    if (!upload || !bind_text(upload.value().get(), 1, upload_id) ||
        !bind_text(upload.value().get(), 2, machine_id))
        return Result<std::string>::failure(
            upload ? workspace_error("DATABASE_ERROR", "无法查询上传完成状态",
                                     "simulator.upload.complete.lookup")
                   : upload.error());
    if (sqlite3_step(upload.value().get()) != SQLITE_ROW)
        return Result<std::string>::failure(
            workspace_error("UPLOAD_REJECTED", "上传任务不存在", "simulator.upload.complete"));
    const std::string event_id = column_text(upload.value().get(), 0);
    const std::string logical_file_id = column_text(upload.value().get(), 1);
    const auto total = static_cast<std::uint64_t>(sqlite3_column_int64(upload.value().get(), 2));
    const auto chunk_size =
        static_cast<std::uint64_t>(sqlite3_column_int64(upload.value().get(), 3));
    const std::string expected = column_text(upload.value().get(), 4);
    const std::string state = column_text(upload.value().get(), 5);
    if (state == "Completed")
        return Result<std::string>::success(
            Json{{"uploadId", upload_id}, {"state", "Completed"}, {"duplicate", true}}.dump());
    const std::uint64_t required_chunks = (total + chunk_size - 1U) / chunk_size;
    auto count = prepare(database_.get(), "SELECT COUNT(*) FROM chunks WHERE upload_id=?1",
                         "simulator.upload.complete.count");
    if (!count || !bind_text(count.value().get(), 1, upload_id) ||
        sqlite3_step(count.value().get()) != SQLITE_ROW)
        return Result<std::string>::failure(
            count ? workspace_error("DATABASE_ERROR", "无法读取上传分块数量",
                                    "simulator.upload.complete.count")
                  : count.error());
    if (static_cast<std::uint64_t>(sqlite3_column_int64(count.value().get(), 0)) != required_chunks)
        return Result<std::string>::failure(workspace_error(
            "UPLOAD_TRANSFER_FAILED", "上传分块尚未完整", "simulator.upload.complete", true));
    const auto partial_path = root_ / ".partial" / std::string{upload_id};
    auto digest = sha256_file(partial_path, stop_token_);
    if (!digest)
        return Result<std::string>::failure(digest.error());
    if (force_mismatch || digest.value() != expected)
        return Result<std::string>::failure(workspace_error("UPLOAD_CHECKSUM_MISMATCH",
                                                            "上传整文件 SHA-256 不匹配",
                                                            "simulator.upload.complete", true));
    std::error_code file_error;
    const auto final_directory = root_ / "events" / std::string{machine_id} / event_id;
    std::filesystem::create_directories(final_directory, file_error);
    if (file_error)
        return Result<std::string>::failure(workspace_error(
            "UPLOAD_TRANSFER_FAILED", "无法创建上传正式目录", "simulator.upload.complete", true));
    const auto final_path = final_directory / logical_file_id;
    if (std::filesystem::exists(final_path, file_error))
    {
        auto final_digest = sha256_file(final_path, stop_token_);
        if (!final_digest || final_digest.value() != expected)
            return Result<std::string>::failure(workspace_error(
                "UPLOAD_REJECTED", "正式逻辑文件已存在且内容冲突", "simulator.upload.complete"));
        std::filesystem::remove(partial_path, file_error);
    }
    else
    {
        std::filesystem::rename(partial_path, final_path, file_error);
        if (file_error)
            return Result<std::string>::failure(workspace_error("UPLOAD_TRANSFER_FAILED",
                                                                "无法原子提交上传文件",
                                                                "simulator.upload.complete", true));
    }
    auto statement = prepare(
        database_.get(), "UPDATE uploads SET state='Completed',completed_at=?2 WHERE upload_id=?1",
        "simulator.upload.complete.update");
    const std::string now = current_utc_timestamp();
    if (!statement || !bind_text(statement.value().get(), 1, upload_id) ||
        !bind_text(statement.value().get(), 2, now))
        return Result<std::string>::failure(
            statement ? workspace_error("DATABASE_ERROR", "无法绑定上传完成状态",
                                        "simulator.upload.complete.update")
                      : statement.error());
    auto updated = step_done(statement.value().get(), "simulator.upload.complete.update");
    if (!updated)
        return Result<std::string>::failure(updated.error());
    return Result<std::string>::success(Json{
        {"uploadId", upload_id},
        {"state", "Completed"},
        {"duplicate", false},
        {"sha256", expected}}.dump());
}

Result<void> Workspace::store_command(const std::string_view command_id,
                                      const std::string_view machine_id,
                                      const std::string_view command_type,
                                      const std::string_view payload_json,
                                      const std::string_view deadline)
{
    auto existing = prepare(database_.get(), R"sql(
SELECT machine_id,command_type,payload_json,deadline FROM commands WHERE command_id=?1)sql",
                            "simulator.command.idempotency");
    if (!existing || !bind_text(existing.value().get(), 1, command_id))
        return Result<void>::failure(existing
                                         ? workspace_error("DATABASE_ERROR", "无法查询命令幂等状态",
                                                           "simulator.command.idempotency")
                                         : existing.error());
    if (sqlite3_step(existing.value().get()) == SQLITE_ROW)
    {
        const bool same = column_text(existing.value().get(), 0) == machine_id &&
                          column_text(existing.value().get(), 1) == command_type &&
                          column_text(existing.value().get(), 2) == payload_json &&
                          column_text(existing.value().get(), 3) == deadline;
        return same ? Result<void>::success()
                    : Result<void>::failure(workspace_error("UPLINK_PROTOCOL_ERROR",
                                                            "commandId 对应持久化内容发生冲突",
                                                            "simulator.command.idempotency"));
    }
    auto statement = prepare(database_.get(), R"sql(
INSERT INTO commands(command_id,machine_id,command_type,payload_json,deadline,state,created_at)
VALUES(?1,?2,?3,?4,?5,'Queued',?6)
)sql",
                             "simulator.command.insert");
    const std::string now = current_utc_timestamp();
    if (!statement || !bind_text(statement.value().get(), 1, command_id) ||
        !bind_text(statement.value().get(), 2, machine_id) ||
        !bind_text(statement.value().get(), 3, command_type) ||
        !bind_text(statement.value().get(), 4, payload_json) ||
        !bind_text(statement.value().get(), 5, deadline) ||
        !bind_text(statement.value().get(), 6, now))
        return Result<void>::failure(
            statement
                ? workspace_error("DATABASE_ERROR", "无法绑定命令写入", "simulator.command.insert")
                : statement.error());
    return step_done(statement.value().get(), "simulator.command.insert");
}

Result<void> Workspace::complete_command(const std::string_view command_id,
                                         const std::string_view result_json)
{
    auto statement = prepare(
        database_.get(), "UPDATE commands SET state='Completed',result_json=?2 WHERE command_id=?1",
        "simulator.command.complete");
    if (!statement || !bind_text(statement.value().get(), 1, command_id) ||
        !bind_text(statement.value().get(), 2, result_json))
        return Result<void>::failure(statement
                                         ? workspace_error("DATABASE_ERROR", "无法绑定命令结果",
                                                           "simulator.command.complete")
                                         : statement.error());
    return step_done(statement.value().get(), "simulator.command.complete");
}

} // namespace paperbreak::uplink::simulator
