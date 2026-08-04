#include "paperbreak/storage/metadata_database.hpp"

#include "paperbreak/storage/event_store.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace paperbreak::storage
{
namespace
{
using Json = nlohmann::json;

constexpr std::size_t maximum_query_offset = 1000000U;
constexpr std::size_t maximum_text_bytes = 4096U;

class SqliteConnection final
{
  public:
    SqliteConnection() = default;
    ~SqliteConnection()
    {
        if (handle_ != nullptr)
            sqlite3_close_v2(handle_);
    }
    SqliteConnection(const SqliteConnection&) = delete;
    SqliteConnection& operator=(const SqliteConnection&) = delete;
    SqliteConnection(SqliteConnection&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr))
    {
    }
    SqliteConnection& operator=(SqliteConnection&& other) noexcept
    {
        if (this != &other)
        {
            if (handle_ != nullptr)
                sqlite3_close_v2(handle_);
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] sqlite3* get() const noexcept
    {
        return handle_;
    }
    [[nodiscard]] sqlite3** put() noexcept
    {
        return &handle_;
    }
    void close() noexcept
    {
        if (handle_ != nullptr)
        {
            sqlite3_close_v2(handle_);
            handle_ = nullptr;
        }
    }

  private:
    sqlite3* handle_{};
};

class Statement final
{
  public:
    explicit Statement(sqlite3_stmt* value) noexcept : value_(value) {}
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

bool valid_text(const std::string_view value, const bool empty_allowed = false) noexcept
{
    return value.size() <= maximum_text_bytes && (empty_allowed || !value.empty());
}

std::string native_code(sqlite3* database, const int fallback)
{
    const auto code = database != nullptr ? sqlite3_extended_errcode(database) : fallback;
    return std::to_string(code);
}

Error database_error(sqlite3* database, const int result, const std::string_view operation,
                     const std::string_view message, const bool migration = false,
                     const bool reconcile = false)
{
    std::string code = "DATABASE_ERROR";
    Severity severity = Severity::error;
    bool retryable = false;
    const auto primary = result & 0xff;
    if (primary == SQLITE_BUSY || primary == SQLITE_LOCKED)
    {
        code = "DATABASE_BUSY";
        severity = Severity::warning;
        retryable = true;
    }
    else if (primary == SQLITE_CORRUPT || primary == SQLITE_NOTADB)
    {
        code = "DATABASE_CORRUPT";
        severity = Severity::critical;
    }
    else if (migration)
    {
        code = "DATABASE_MIGRATION_FAILED";
        severity = Severity::critical;
    }
    else if (reconcile)
    {
        code = "DATABASE_RECONCILE_FAILED";
        retryable = true;
    }
    Error error = make_error(std::move(code), severity, std::string{message}, "storage",
                             std::string{operation}, retryable);
    error.native_domain = "sqlite";
    error.native_code = native_code(database, result);
    if (database != nullptr && sqlite3_errmsg(database) != nullptr)
        error.details.push_back({.key = "sqliteMessage", .value = sqlite3_errmsg(database)});
    return error;
}

Error config_error(const std::string_view message, const std::string_view operation)
{
    return make_error("SYS_CONFIG_INVALID", Severity::error, std::string{message}, "storage",
                      std::string{operation});
}

Error reconcile_error(const std::string_view message, const std::string_view operation,
                      const std::filesystem::path& path = {})
{
    Error error = make_error("DATABASE_RECONCILE_FAILED", Severity::error, std::string{message},
                             "storage", std::string{operation}, true);
    if (!path.empty())
        error.details.push_back({.key = "path", .value = path.string()});
    return error;
}

std::string utf8_path(const std::filesystem::path& path)
{
    const auto wide = path.wstring();
    if (wide.empty())
        return {};
    const auto size =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                            static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return {};
    std::string value(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                            static_cast<int>(wide.size()), value.data(), size, nullptr,
                            nullptr) != size)
        return {};
    return value;
}

Result<void> execute(sqlite3* database, const std::string_view sql,
                     const std::string_view operation, const bool migration = false)
{
    char* error_message = nullptr;
    const auto result =
        sqlite3_exec(database, std::string{sql}.c_str(), nullptr, nullptr, &error_message);
    if (result == SQLITE_OK)
        return Result<void>::success();
    auto error = database_error(database, result, operation, "SQLite 语句执行失败", migration);
    if (error_message != nullptr)
    {
        error.details.push_back({.key = "sqliteExecMessage", .value = error_message});
        sqlite3_free(error_message);
    }
    return Result<void>::failure(std::move(error));
}

Result<Statement> prepare(sqlite3* database, const std::string_view sql,
                          const std::string_view operation, const bool reconcile = false)
{
    sqlite3_stmt* statement = nullptr;
    const auto result =
        sqlite3_prepare_v2(database, sql.data(), static_cast<int>(sql.size()), &statement, nullptr);
    if (result != SQLITE_OK)
        return Result<Statement>::failure(
            database_error(database, result, operation, "SQLite 语句准备失败", false, reconcile));
    return Result<Statement>::success(Statement{statement});
}

Result<void> step_done(sqlite3* database, sqlite3_stmt* statement, const std::string_view operation,
                       const bool reconcile = false)
{
    const auto result = sqlite3_step(statement);
    if (result == SQLITE_DONE)
        return Result<void>::success();
    return Result<void>::failure(
        database_error(database, result, operation, "SQLite 写事务执行失败", false, reconcile));
}

bool bind_text(sqlite3_stmt* statement, int& index, const std::string_view value)
{
    return sqlite3_bind_text(statement, index++, value.data(), static_cast<int>(value.size()),
                             SQLITE_TRANSIENT) == SQLITE_OK;
}

bool bind_int64(sqlite3_stmt* statement, int& index, const std::int64_t value)
{
    return sqlite3_bind_int64(statement, index++, value) == SQLITE_OK;
}

bool bind_double(sqlite3_stmt* statement, int& index, const double value)
{
    return sqlite3_bind_double(statement, index++, value) == SQLITE_OK;
}

bool bind_null(sqlite3_stmt* statement, int& index)
{
    return sqlite3_bind_null(statement, index++) == SQLITE_OK;
}

Result<DatabaseIntegrityReport> quick_check(sqlite3* database)
{
    auto prepared = prepare(database, "PRAGMA quick_check(1)", "database.integrity");
    if (!prepared)
        return Result<DatabaseIntegrityReport>::failure(std::move(prepared).error());
    auto statement = std::move(prepared).value();
    const auto result = sqlite3_step(statement.get());
    if (result != SQLITE_ROW)
        return Result<DatabaseIntegrityReport>::failure(database_error(
            database, result, "database.integrity", "SQLite 完整性检查无法读取结果"));
    const auto* text = sqlite3_column_text(statement.get(), 0);
    const std::string detail =
        text == nullptr ? std::string{} : reinterpret_cast<const char*>(text);
    if (detail == "ok")
        return Result<DatabaseIntegrityReport>::success({.healthy = true, .detail = detail});
    Error error = database_error(database, SQLITE_CORRUPT, "database.integrity",
                                 "SQLite 完整性检查报告数据库损坏");
    error.details.push_back({.key = "quickCheck", .value = detail});
    return Result<DatabaseIntegrityReport>::failure(std::move(error));
}

Result<std::uint32_t> read_schema_version(sqlite3* database)
{
    auto prepared = prepare(database, "PRAGMA user_version", "database.version");
    if (!prepared)
        return Result<std::uint32_t>::failure(std::move(prepared).error());
    auto statement = std::move(prepared).value();
    const auto result = sqlite3_step(statement.get());
    if (result != SQLITE_ROW)
        return Result<std::uint32_t>::failure(
            database_error(database, result, "database.version", "无法读取数据库 schema 版本"));
    const auto version = sqlite3_column_int64(statement.get(), 0);
    if (version < 0 || version > std::numeric_limits<std::uint32_t>::max())
        return Result<std::uint32_t>::failure(
            database_error(database, SQLITE_SCHEMA, "database.version", "数据库 schema 版本无效"));
    return Result<std::uint32_t>::success(static_cast<std::uint32_t>(version));
}

constexpr std::string_view schema_v1 = R"sql(
CREATE TABLE IF NOT EXISTS events(
  event_id TEXT PRIMARY KEY NOT NULL,
  event_schema_version INTEGER NOT NULL CHECK(event_schema_version > 0),
  event_state TEXT NOT NULL,
  candidate_time_utc_ms INTEGER NOT NULL,
  confirmed_time_utc_ms INTEGER,
  start_time_utc_ms INTEGER NOT NULL,
  end_time_utc_ms INTEGER NOT NULL,
  trigger_camera_id TEXT NOT NULL,
  trigger_frame_number INTEGER NOT NULL CHECK(trigger_frame_number >= 0),
  trigger_reason TEXT NOT NULL,
  confidence REAL NOT NULL CHECK(confidence >= 0.0 AND confidence <= 1.0),
  pre_event_ms INTEGER NOT NULL CHECK(pre_event_ms >= 0),
  post_event_ms INTEGER NOT NULL CHECK(post_event_ms >= 0),
  algorithm_name TEXT NOT NULL,
  algorithm_version TEXT NOT NULL,
  config_version TEXT NOT NULL,
  machine_id TEXT NOT NULL,
  production_line_id TEXT NOT NULL,
  paper_type TEXT NOT NULL,
  paper_speed REAL,
  upload_state TEXT NOT NULL,
  time_quality TEXT NOT NULL,
  relative_directory TEXT NOT NULL UNIQUE,
  storage_state TEXT NOT NULL CHECK(storage_state IN ('Present','Missing','Damaged')),
  window_complete INTEGER NOT NULL CHECK(window_complete IN (0,1)),
  truncated_by_maximum_duration INTEGER NOT NULL CHECK(truncated_by_maximum_duration IN (0,1)),
  stopped_early INTEGER NOT NULL CHECK(stopped_early IN (0,1)),
  indexed_at_utc_ms INTEGER NOT NULL
) STRICT;
CREATE TABLE IF NOT EXISTS event_cameras(
  event_id TEXT NOT NULL REFERENCES events(event_id) ON DELETE CASCADE,
  camera_id TEXT NOT NULL,
  is_trigger INTEGER NOT NULL CHECK(is_trigger IN (0,1)),
  PRIMARY KEY(event_id,camera_id)
) STRICT;
CREATE TABLE IF NOT EXISTS key_frames(
  event_id TEXT NOT NULL REFERENCES events(event_id) ON DELETE CASCADE,
  ordinal INTEGER NOT NULL CHECK(ordinal >= 0),
  path TEXT NOT NULL,
  camera_id TEXT NOT NULL,
  camera_frame_number INTEGER NOT NULL CHECK(camera_frame_number >= 0),
  sequence_number INTEGER NOT NULL CHECK(sequence_number >= 0),
  wall_clock_time_utc_ms INTEGER NOT NULL,
  width INTEGER NOT NULL CHECK(width > 0),
  height INTEGER NOT NULL CHECK(height > 0),
  stride INTEGER NOT NULL CHECK(stride > 0),
  pixel_format TEXT NOT NULL,
  reasons_json TEXT NOT NULL,
  checksum TEXT NOT NULL,
  size_bytes INTEGER NOT NULL CHECK(size_bytes > 0),
  PRIMARY KEY(event_id,ordinal),
  UNIQUE(event_id,path)
) STRICT;
CREATE TABLE IF NOT EXISTS event_files(
  event_id TEXT NOT NULL REFERENCES events(event_id) ON DELETE CASCADE,
  path TEXT NOT NULL,
  file_kind TEXT NOT NULL,
  camera_id TEXT,
  camera_frame_number INTEGER,
  sequence_number INTEGER,
  wall_clock_time_utc_ms INTEGER,
  checksum TEXT NOT NULL,
  size_bytes INTEGER NOT NULL CHECK(size_bytes > 0),
  attributes_json TEXT NOT NULL,
  PRIMARY KEY(event_id,path)
) STRICT;
CREATE TABLE IF NOT EXISTS upload_jobs(
  job_id INTEGER PRIMARY KEY,
  event_id TEXT NOT NULL REFERENCES events(event_id) ON DELETE CASCADE,
  state TEXT NOT NULL,
  attempts INTEGER NOT NULL DEFAULT 0 CHECK(attempts >= 0),
  next_attempt_utc_ms INTEGER,
  checkpoint_json TEXT NOT NULL DEFAULT '{}',
  last_error_code TEXT,
  updated_at_utc_ms INTEGER NOT NULL,
  UNIQUE(event_id)
) STRICT;
CREATE TABLE IF NOT EXISTS device_status_history(
  sequence INTEGER PRIMARY KEY,
  source_id TEXT NOT NULL,
  state TEXT NOT NULL,
  observed_at_utc_ms INTEGER NOT NULL,
  details_json TEXT NOT NULL DEFAULT '{}'
) STRICT;
CREATE TABLE IF NOT EXISTS config_history(
  config_revision INTEGER PRIMARY KEY CHECK(config_revision > 0),
  config_schema_version INTEGER NOT NULL CHECK(config_schema_version > 0),
  modified_at_utc_ms INTEGER NOT NULL,
  actor TEXT NOT NULL,
  checksum TEXT NOT NULL,
  config_json TEXT NOT NULL
) STRICT;
CREATE TABLE IF NOT EXISTS alarm_history(
  alarm_id TEXT NOT NULL,
  occurrence INTEGER NOT NULL CHECK(occurrence > 0),
  source_id TEXT NOT NULL,
  category TEXT NOT NULL,
  severity TEXT NOT NULL,
  state TEXT NOT NULL,
  raised_at_utc_ms INTEGER NOT NULL,
  cleared_at_utc_ms INTEGER,
  acknowledged_at_utc_ms INTEGER,
  details_json TEXT NOT NULL DEFAULT '{}',
  PRIMARY KEY(alarm_id,occurrence)
) STRICT;
CREATE TABLE IF NOT EXISTS audit_logs(
  audit_id TEXT PRIMARY KEY NOT NULL,
  occurred_at_utc_ms INTEGER NOT NULL,
  actor TEXT NOT NULL,
  action TEXT NOT NULL,
  target_type TEXT NOT NULL,
  target_id TEXT NOT NULL,
  outcome TEXT NOT NULL,
  details_json TEXT NOT NULL DEFAULT '{}'
) STRICT;
CREATE INDEX IF NOT EXISTS idx_events_candidate ON events(candidate_time_utc_ms DESC,event_id DESC);
CREATE INDEX IF NOT EXISTS idx_events_state_time ON events(event_state,candidate_time_utc_ms DESC);
CREATE INDEX IF NOT EXISTS idx_event_cameras_camera ON event_cameras(camera_id,event_id);
CREATE INDEX IF NOT EXISTS idx_upload_jobs_state_time ON upload_jobs(state,next_attempt_utc_ms);
CREATE INDEX IF NOT EXISTS idx_device_status_time ON device_status_history(observed_at_utc_ms DESC);
CREATE INDEX IF NOT EXISTS idx_alarm_history_time ON alarm_history(raised_at_utc_ms DESC);
CREATE INDEX IF NOT EXISTS idx_audit_logs_time ON audit_logs(occurred_at_utc_ms DESC);
)sql";

constexpr std::string_view schema_v2 = R"sql(
CREATE TABLE IF NOT EXISTS event_retention(
  event_id TEXT PRIMARY KEY NOT NULL REFERENCES events(event_id) ON DELETE CASCADE,
  locked INTEGER NOT NULL DEFAULT 0 CHECK(locked IN (0,1)),
  deletion_allowed INTEGER NOT NULL DEFAULT 0 CHECK(deletion_allowed IN (0,1)),
  deletion_state TEXT NOT NULL DEFAULT 'Active'
    CHECK(deletion_state IN ('Active','DeletePending','DeleteFailed','Deleted')),
  deletion_relative_path TEXT NOT NULL DEFAULT '',
  manifest_size_bytes INTEGER NOT NULL DEFAULT 0 CHECK(manifest_size_bytes >= 0),
  last_error TEXT NOT NULL DEFAULT '',
  updated_at_utc_ms INTEGER NOT NULL DEFAULT 0,
  deleted_at_utc_ms INTEGER,
  CHECK(deletion_state <> 'Deleted' OR deleted_at_utc_ms IS NOT NULL)
) STRICT;
CREATE INDEX IF NOT EXISTS idx_event_retention_cleanup
  ON event_retention(deletion_state,locked,deletion_allowed,event_id);
INSERT OR IGNORE INTO event_retention(event_id) SELECT event_id FROM events;
)sql";

Result<void> migrate_to_v1(sqlite3* database)
{
    auto begun = execute(database, "BEGIN IMMEDIATE", "database.migrate.begin", true);
    if (!begun)
        return begun;
    auto schema = execute(database, schema_v1, "database.migrate.schema-v1", true);
    if (schema)
        schema = execute(database, "PRAGMA user_version=1", "database.migrate.version", true);
    if (schema)
        schema = execute(database, "COMMIT", "database.migrate.commit", true);
    if (!schema)
    {
        static_cast<void>(execute(database, "ROLLBACK", "database.migrate.rollback", true));
        return schema;
    }
    return Result<void>::success();
}

Result<void> migrate_to_v2(sqlite3* database)
{
    auto begun = execute(database, "BEGIN IMMEDIATE", "database.migrate.begin", true);
    if (!begun)
        return begun;
    auto schema = execute(database, schema_v2, "database.migrate.schema-v2", true);
    if (schema)
        schema = execute(database, "PRAGMA user_version=2", "database.migrate.version", true);
    if (schema)
        schema = execute(database, "COMMIT", "database.migrate.commit", true);
    if (!schema)
    {
        static_cast<void>(execute(database, "ROLLBACK", "database.migrate.rollback", true));
        return schema;
    }
    return Result<void>::success();
}

Result<void> reserve_file(const std::filesystem::path& path)
{
    const auto handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        Error error = make_error("DATABASE_ERROR", Severity::error, "无法创建唯一数据库备份文件",
                                 "storage", "database.backup.reserve");
        error.native_domain = "win32";
        error.native_code = std::to_string(GetLastError());
        error.details.push_back({.key = "path", .value = path.string()});
        return Result<void>::failure(std::move(error));
    }
    CloseHandle(handle);
    return Result<void>::success();
}

Result<void> backup_connection(sqlite3* source, const std::filesystem::path& destination,
                               const int busy_timeout_ms)
{
    auto reserved = reserve_file(destination);
    if (!reserved)
        return reserved;
    SqliteConnection target;
    const auto destination_utf8 = utf8_path(destination);
    const auto open_result =
        sqlite3_open_v2(destination_utf8.c_str(), target.put(),
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (open_result != SQLITE_OK)
    {
        target.close();
        DeleteFileW(destination.c_str());
        return Result<void>::failure(database_error(
            target.get(), open_result, "database.backup.open", "无法打开数据库备份目标"));
    }
    sqlite3_busy_timeout(target.get(), busy_timeout_ms);
    sqlite3_backup* backup = sqlite3_backup_init(target.get(), "main", source, "main");
    if (backup == nullptr)
    {
        auto error = database_error(target.get(), sqlite3_errcode(target.get()),
                                    "database.backup.initialize", "无法初始化 SQLite 备份");
        target.close();
        DeleteFileW(destination.c_str());
        return Result<void>::failure(std::move(error));
    }
    const auto step_result = sqlite3_backup_step(backup, -1);
    const auto finish_result = sqlite3_backup_finish(backup);
    if (step_result != SQLITE_DONE || finish_result != SQLITE_OK)
    {
        const auto result = finish_result != SQLITE_OK ? finish_result : step_result;
        auto error =
            database_error(target.get(), result, "database.backup.copy", "SQLite 一致备份失败");
        target.close();
        DeleteFileW(destination.c_str());
        return Result<void>::failure(std::move(error));
    }
    auto checked = quick_check(target.get());
    if (!checked)
    {
        auto error = std::move(checked).error();
        target.close();
        DeleteFileW(destination.c_str());
        return Result<void>::failure(std::move(error));
    }
    return Result<void>::success();
}

Result<SqliteConnection> open_connection(const std::filesystem::path& path, const int flags,
                                         const int busy_timeout_ms)
{
    SqliteConnection connection;
    const auto path_utf8 = utf8_path(path);
    if (path_utf8.empty())
        return Result<SqliteConnection>::failure(
            config_error("SQLite 路径无法转换为 UTF-8", "database.open.path"));
    const auto result = sqlite3_open_v2(path_utf8.c_str(), connection.put(), flags, nullptr);
    if (result != SQLITE_OK)
        return Result<SqliteConnection>::failure(
            database_error(connection.get(), result, "database.open", "无法打开 SQLite 数据库"));
    sqlite3_extended_result_codes(connection.get(), 1);
    sqlite3_busy_timeout(connection.get(), busy_timeout_ms);
    return Result<SqliteConnection>::success(std::move(connection));
}

std::optional<std::int64_t> parse_utc_milliseconds(const std::string_view value) noexcept
{
    if (value.size() != 24U || value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
        value[13] != ':' || value[16] != ':' || value[19] != '.' || value[23] != 'Z')
        return std::nullopt;
    const auto digits = [&](const std::size_t offset, const std::size_t count) {
        int number = 0;
        for (std::size_t index = 0U; index < count; ++index)
        {
            const auto character = value[offset + index];
            if (character < '0' || character > '9')
                return -1;
            number = number * 10 + (character - '0');
        }
        return number;
    };
    const auto year_value = digits(0U, 4U);
    const auto month_value = digits(5U, 2U);
    const auto day_value = digits(8U, 2U);
    const auto hour = digits(11U, 2U);
    const auto minute = digits(14U, 2U);
    const auto second = digits(17U, 2U);
    const auto millisecond = digits(20U, 3U);
    const std::chrono::year_month_day date{std::chrono::year{year_value},
                                           std::chrono::month{static_cast<unsigned>(month_value)},
                                           std::chrono::day{static_cast<unsigned>(day_value)}};
    if (!date.ok() || hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 ||
        second > 59 || millisecond < 0)
        return std::nullopt;
    const auto instant = std::chrono::sys_days{date} + std::chrono::hours{hour} +
                         std::chrono::minutes{minute} + std::chrono::seconds{second} +
                         std::chrono::milliseconds{millisecond};
    return instant.time_since_epoch().count();
}

std::int64_t current_epoch_milliseconds() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::optional<std::int64_t> json_time(const Json& value, const std::string_view key)
{
    const auto iterator = value.find(key);
    if (iterator == value.end() || !iterator->is_string())
        return std::nullopt;
    return parse_utc_milliseconds(iterator->get_ref<const std::string&>());
}

struct IndexedManifest final
{
    Json value;
    std::size_t manifest_size_bytes{};
    EventMetadataRecord event;
    std::int64_t pre_event_ms{};
    std::int64_t post_event_ms{};
    std::string algorithm_name;
    std::string algorithm_version;
    std::string config_version;
    std::string machine_id;
    std::string production_line_id;
    std::string paper_type;
    std::optional<double> paper_speed;
    std::string time_quality;
    bool window_complete{};
    bool truncated{};
    bool stopped_early{};
};

Result<IndexedManifest> parse_index_manifest(const std::string& manifest_text)
{
    try
    {
        Json value = Json::parse(manifest_text);
        const auto candidate = json_time(value, "candidateTime");
        const auto start = json_time(value, "startTime");
        const auto end = json_time(value, "endTime");
        std::optional<std::int64_t> confirmed;
        if (!value.at("confirmedTime").is_null())
            confirmed = json_time(value, "confirmedTime");
        const auto trigger_frame = value.at("triggerFrameNumber").get<std::uint64_t>();
        const auto pre_seconds = value.at("preEventSeconds").get<double>();
        const auto post_seconds = value.at("postEventSeconds").get<double>();
        if (!candidate || !start || !end || (!value.at("confirmedTime").is_null() && !confirmed) ||
            trigger_frame > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
            !std::isfinite(pre_seconds) || !std::isfinite(post_seconds) ||
            pre_seconds > static_cast<double>(std::numeric_limits<std::int64_t>::max()) / 1000.0 ||
            post_seconds > static_cast<double>(std::numeric_limits<std::int64_t>::max()) / 1000.0)
            return Result<IndexedManifest>::failure(
                reconcile_error("事件 manifest 时间或数值字段无法索引", "database.index.parse"));

        IndexedManifest indexed;
        indexed.manifest_size_bytes = manifest_text.size();
        indexed.event = {.event_id = value.at("eventId").get<std::string>(),
                         .event_schema_version = value.at("schemaVersion").get<std::uint32_t>(),
                         .event_state = value.at("eventState").get<std::string>(),
                         .candidate_time_utc_ms = *candidate,
                         .confirmed_time_utc_ms = confirmed,
                         .start_time_utc_ms = *start,
                         .end_time_utc_ms = *end,
                         .camera_ids = value.at("cameraIds").get<std::vector<std::string>>(),
                         .trigger_camera_id = value.at("triggerCameraId").get<std::string>(),
                         .trigger_frame_number = trigger_frame,
                         .trigger_reason = value.at("triggerReason").get<std::string>(),
                         .confidence = value.at("confidence").get<double>(),
                         .upload_state = value.at("uploadState").get<std::string>(),
                         .storage_state = "Present",
                         .relative_directory =
                             value.at("destinationRelativePath").get<std::string>()};
        indexed.pre_event_ms = static_cast<std::int64_t>(std::llround(pre_seconds * 1000.0));
        indexed.post_event_ms = static_cast<std::int64_t>(std::llround(post_seconds * 1000.0));
        indexed.algorithm_name = value.at("algorithmName").get<std::string>();
        indexed.algorithm_version = value.at("algorithmVersion").get<std::string>();
        indexed.config_version = value.at("configVersion").get<std::string>();
        indexed.machine_id = value.at("machineId").get<std::string>();
        indexed.production_line_id = value.at("productionLineId").get<std::string>();
        indexed.paper_type = value.at("paperType").get<std::string>();
        if (!value.at("paperSpeed").is_null())
            indexed.paper_speed = value.at("paperSpeed").get<double>();
        indexed.time_quality = value.at("timeQuality").get<std::string>();
        indexed.window_complete = value.at("windowComplete").get<bool>();
        indexed.truncated = value.at("truncatedByMaximumDuration").get<bool>();
        indexed.stopped_early = value.at("stoppedEarly").get<bool>();
        indexed.value = std::move(value);
        return Result<IndexedManifest>::success(std::move(indexed));
    }
    catch (const std::exception&)
    {
        return Result<IndexedManifest>::failure(
            reconcile_error("事件 manifest 无法转换为数据库索引", "database.index.parse"));
    }
}

Result<void> bind_failure(sqlite3* database, const std::string_view operation)
{
    return Result<void>::failure(database_error(database, sqlite3_errcode(database), operation,
                                                "SQLite 参数绑定失败", false, true));
}

Result<void> index_manifest(sqlite3* database, const IndexedManifest& indexed)
{
    constexpr std::string_view event_sql = R"sql(
INSERT INTO events(event_id,event_schema_version,event_state,candidate_time_utc_ms,
 confirmed_time_utc_ms,start_time_utc_ms,end_time_utc_ms,trigger_camera_id,
 trigger_frame_number,trigger_reason,confidence,pre_event_ms,post_event_ms,algorithm_name,
 algorithm_version,config_version,machine_id,production_line_id,paper_type,paper_speed,
 upload_state,time_quality,relative_directory,storage_state,window_complete,
 truncated_by_maximum_duration,stopped_early,indexed_at_utc_ms)
VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
ON CONFLICT(event_id) DO UPDATE SET
 event_schema_version=excluded.event_schema_version,event_state=excluded.event_state,
 candidate_time_utc_ms=excluded.candidate_time_utc_ms,
 confirmed_time_utc_ms=excluded.confirmed_time_utc_ms,start_time_utc_ms=excluded.start_time_utc_ms,
 end_time_utc_ms=excluded.end_time_utc_ms,trigger_camera_id=excluded.trigger_camera_id,
 trigger_frame_number=excluded.trigger_frame_number,trigger_reason=excluded.trigger_reason,
 confidence=excluded.confidence,pre_event_ms=excluded.pre_event_ms,
 post_event_ms=excluded.post_event_ms,algorithm_name=excluded.algorithm_name,
 algorithm_version=excluded.algorithm_version,config_version=excluded.config_version,
 machine_id=excluded.machine_id,production_line_id=excluded.production_line_id,
 paper_type=excluded.paper_type,paper_speed=excluded.paper_speed,upload_state=excluded.upload_state,
 time_quality=excluded.time_quality,relative_directory=excluded.relative_directory,
 storage_state='Present',window_complete=excluded.window_complete,
 truncated_by_maximum_duration=excluded.truncated_by_maximum_duration,
 stopped_early=excluded.stopped_early,indexed_at_utc_ms=excluded.indexed_at_utc_ms
)sql";
    auto prepared = prepare(database, event_sql, "database.index.event", true);
    if (!prepared)
        return Result<void>::failure(std::move(prepared).error());
    auto statement = std::move(prepared).value();
    int parameter = 1;
    const auto& event = indexed.event;
    bool bound = bind_text(statement.get(), parameter, event.event_id) &&
                 bind_int64(statement.get(), parameter, event.event_schema_version) &&
                 bind_text(statement.get(), parameter, event.event_state) &&
                 bind_int64(statement.get(), parameter, event.candidate_time_utc_ms);
    bound = bound && (event.confirmed_time_utc_ms
                          ? bind_int64(statement.get(), parameter, *event.confirmed_time_utc_ms)
                          : bind_null(statement.get(), parameter));
    bound = bound && bind_int64(statement.get(), parameter, event.start_time_utc_ms) &&
            bind_int64(statement.get(), parameter, event.end_time_utc_ms) &&
            bind_text(statement.get(), parameter, event.trigger_camera_id) &&
            bind_int64(statement.get(), parameter,
                       static_cast<std::int64_t>(event.trigger_frame_number)) &&
            bind_text(statement.get(), parameter, event.trigger_reason) &&
            bind_double(statement.get(), parameter, event.confidence) &&
            bind_int64(statement.get(), parameter, indexed.pre_event_ms) &&
            bind_int64(statement.get(), parameter, indexed.post_event_ms) &&
            bind_text(statement.get(), parameter, indexed.algorithm_name) &&
            bind_text(statement.get(), parameter, indexed.algorithm_version) &&
            bind_text(statement.get(), parameter, indexed.config_version) &&
            bind_text(statement.get(), parameter, indexed.machine_id) &&
            bind_text(statement.get(), parameter, indexed.production_line_id) &&
            bind_text(statement.get(), parameter, indexed.paper_type);
    bound = bound &&
            (indexed.paper_speed ? bind_double(statement.get(), parameter, *indexed.paper_speed)
                                 : bind_null(statement.get(), parameter));
    bound = bound && bind_text(statement.get(), parameter, event.upload_state) &&
            bind_text(statement.get(), parameter, indexed.time_quality) &&
            bind_text(statement.get(), parameter, event.relative_directory.generic_string()) &&
            bind_text(statement.get(), parameter, "Present") &&
            bind_int64(statement.get(), parameter, indexed.window_complete ? 1 : 0) &&
            bind_int64(statement.get(), parameter, indexed.truncated ? 1 : 0) &&
            bind_int64(statement.get(), parameter, indexed.stopped_early ? 1 : 0) &&
            bind_int64(statement.get(), parameter, current_epoch_milliseconds());
    if (!bound)
        return bind_failure(database, "database.index.event.bind");
    auto stepped = step_done(database, statement.get(), "database.index.event", true);
    if (!stepped)
        return stepped;

    auto retention = prepare(database, R"sql(
INSERT INTO event_retention(event_id,manifest_size_bytes) VALUES(?,?)
 ON CONFLICT(event_id) DO UPDATE SET manifest_size_bytes=excluded.manifest_size_bytes
)sql",
                             "database.index.retention", true);
    if (!retention)
        return Result<void>::failure(std::move(retention).error());
    auto retention_statement = std::move(retention).value();
    int retention_index = 1;
    if (!bind_text(retention_statement.get(), retention_index, event.event_id) ||
        !bind_int64(retention_statement.get(), retention_index,
                    static_cast<std::int64_t>(indexed.manifest_size_bytes)))
        return bind_failure(database, "database.index.retention.bind");
    auto retention_inserted =
        step_done(database, retention_statement.get(), "database.index.retention", true);
    if (!retention_inserted)
        return retention_inserted;

    for (const auto table : {"event_cameras", "key_frames", "event_files"})
    {
        const auto sql = "DELETE FROM " + std::string{table} + " WHERE event_id=?";
        auto deletion = prepare(database, sql, "database.index.replace", true);
        if (!deletion)
            return Result<void>::failure(std::move(deletion).error());
        auto delete_statement = std::move(deletion).value();
        int index = 1;
        if (!bind_text(delete_statement.get(), index, event.event_id))
            return bind_failure(database, "database.index.replace.bind");
        auto deleted = step_done(database, delete_statement.get(), "database.index.replace", true);
        if (!deleted)
            return deleted;
    }

    for (const auto& camera_id : event.camera_ids)
    {
        auto camera = prepare(
            database, "INSERT INTO event_cameras(event_id,camera_id,is_trigger) VALUES(?,?,?)",
            "database.index.camera", true);
        if (!camera)
            return Result<void>::failure(std::move(camera).error());
        auto camera_statement = std::move(camera).value();
        int index = 1;
        if (!bind_text(camera_statement.get(), index, event.event_id) ||
            !bind_text(camera_statement.get(), index, camera_id) ||
            !bind_int64(camera_statement.get(), index,
                        camera_id == event.trigger_camera_id ? 1 : 0))
            return bind_failure(database, "database.index.camera.bind");
        auto inserted = step_done(database, camera_statement.get(), "database.index.camera", true);
        if (!inserted)
            return inserted;
    }

    const auto& checksums = indexed.value.at("fileChecksums");
    const auto& sizes = indexed.value.at("fileSizes");
    const auto insert_file = [&](const std::string_view path, const std::string_view kind,
                                 const Json* item) -> Result<void> {
        auto file = prepare(database, R"sql(
INSERT INTO event_files(event_id,path,file_kind,camera_id,camera_frame_number,sequence_number,
 wall_clock_time_utc_ms,checksum,size_bytes,attributes_json) VALUES(?,?,?,?,?,?,?,?,?,?)
)sql",
                            "database.index.file", true);
        if (!file)
            return Result<void>::failure(std::move(file).error());
        auto file_statement = std::move(file).value();
        int index = 1;
        bool ok = bind_text(file_statement.get(), index, event.event_id) &&
                  bind_text(file_statement.get(), index, path) &&
                  bind_text(file_statement.get(), index, kind);
        if (item == nullptr)
        {
            ok = ok && bind_null(file_statement.get(), index) &&
                 bind_null(file_statement.get(), index) && bind_null(file_statement.get(), index) &&
                 bind_null(file_statement.get(), index);
        }
        else
        {
            const auto wall_time = json_time(*item, "wallClockTime");
            const auto camera_frame = item->at("cameraFrameNumber").get<std::uint64_t>();
            const auto sequence = item->at("sequenceNumber").get<std::uint64_t>();
            if (!wall_time ||
                camera_frame >
                    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
                sequence > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                return Result<void>::failure(
                    reconcile_error("事件文件时间或帧号无法索引", "database.index.file.validate"));
            ok = ok &&
                 bind_text(file_statement.get(), index,
                           item->at("cameraId").get_ref<const std::string&>()) &&
                 bind_int64(file_statement.get(), index, static_cast<std::int64_t>(camera_frame)) &&
                 bind_int64(file_statement.get(), index, static_cast<std::int64_t>(sequence)) &&
                 bind_int64(file_statement.get(), index, *wall_time);
        }
        const auto path_string = std::string{path};
        ok = ok &&
             bind_text(file_statement.get(), index,
                       checksums.at(path_string).get_ref<const std::string&>()) &&
             bind_int64(file_statement.get(), index,
                        static_cast<std::int64_t>(sizes.at(path_string).get<std::uint64_t>())) &&
             bind_text(file_statement.get(), index, item == nullptr ? "{}" : item->dump());
        if (!ok)
            return bind_failure(database, "database.index.file.bind");
        return step_done(database, file_statement.get(), "database.index.file", true);
    };

    auto metadata = insert_file("event.json", "Metadata", nullptr);
    if (!metadata)
        return metadata;
    for (const auto& raw : indexed.value.at("rawFiles"))
    {
        auto inserted = insert_file(raw.at("path").get_ref<const std::string&>(), "RawFrame", &raw);
        if (!inserted)
            return inserted;
    }
    std::size_t ordinal = 0U;
    for (const auto& key_frame : indexed.value.at("keyFrames"))
    {
        const auto path = key_frame.at("path").get<std::string>();
        auto file_insert = insert_file(path, "KeyFrame", &key_frame);
        if (!file_insert)
            return file_insert;
        auto key = prepare(database, R"sql(
INSERT INTO key_frames(event_id,ordinal,path,camera_id,camera_frame_number,sequence_number,
 wall_clock_time_utc_ms,width,height,stride,pixel_format,reasons_json,checksum,size_bytes)
VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)
)sql",
                           "database.index.keyframe", true);
        if (!key)
            return Result<void>::failure(std::move(key).error());
        auto key_statement = std::move(key).value();
        const auto wall_time = json_time(key_frame, "wallClockTime");
        const auto camera_frame = key_frame.at("cameraFrameNumber").get<std::uint64_t>();
        const auto sequence = key_frame.at("sequenceNumber").get<std::uint64_t>();
        if (!wall_time ||
            camera_frame > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
            sequence > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            return Result<void>::failure(
                reconcile_error("关键帧字段无法索引", "database.index.keyframe.validate"));
        int index = 1;
        const bool ok =
            bind_text(key_statement.get(), index, event.event_id) &&
            bind_int64(key_statement.get(), index, static_cast<std::int64_t>(ordinal++)) &&
            bind_text(key_statement.get(), index, path) &&
            bind_text(key_statement.get(), index,
                      key_frame.at("cameraId").get_ref<const std::string&>()) &&
            bind_int64(key_statement.get(), index, static_cast<std::int64_t>(camera_frame)) &&
            bind_int64(key_statement.get(), index, static_cast<std::int64_t>(sequence)) &&
            bind_int64(key_statement.get(), index, *wall_time) &&
            bind_int64(key_statement.get(), index, key_frame.at("width").get<std::int64_t>()) &&
            bind_int64(key_statement.get(), index, key_frame.at("height").get<std::int64_t>()) &&
            bind_int64(key_statement.get(), index, key_frame.at("stride").get<std::int64_t>()) &&
            bind_text(key_statement.get(), index,
                      key_frame.at("pixelFormat").get_ref<const std::string&>()) &&
            bind_text(key_statement.get(), index, key_frame.at("reasons").dump()) &&
            bind_text(key_statement.get(), index,
                      checksums.at(path).get_ref<const std::string&>()) &&
            bind_int64(key_statement.get(), index,
                       static_cast<std::int64_t>(sizes.at(path).get<std::uint64_t>()));
        if (!ok)
            return bind_failure(database, "database.index.keyframe.bind");
        auto inserted = step_done(database, key_statement.get(), "database.index.keyframe", true);
        if (!inserted)
            return inserted;
    }
    return Result<void>::success();
}

std::string column_text(sqlite3_stmt* statement, const int column)
{
    const auto* value = sqlite3_column_text(statement, column);
    return value == nullptr ? std::string{} : reinterpret_cast<const char*>(value);
}

bool directory_component(const std::filesystem::directory_entry& entry,
                         const std::size_t length) noexcept
{
    std::error_code error;
    if (!entry.is_directory(error) || error)
        return false;
    const auto value = entry.path().filename().string();
    return value.size() == length && std::ranges::all_of(value, [](const unsigned char character) {
               return character >= '0' && character <= '9';
           });
}

} // namespace

struct MetadataDatabaseImpl final
{
    MetadataDatabaseOptions options;
    SqliteConnection database;
    MetadataDatabaseOpenReport report;
    mutable std::mutex mutex;
};

Result<std::unique_ptr<EventMetadataDatabase>> EventMetadataDatabase::open(
    MetadataDatabaseOptions options)
{
    std::error_code error;
    if (options.database_path.empty() || options.event_root.empty() ||
        options.busy_timeout.count() <= 0 || options.busy_timeout.count() > 60000 ||
        options.maximum_reconcile_events == 0U || options.maximum_reconcile_events > 100000U ||
        options.maximum_manifest_bytes == 0U ||
        options.maximum_manifest_bytes > 64U * 1024U * 1024U)
        return Result<std::unique_ptr<EventMetadataDatabase>>::failure(
            config_error("SQLite 元数据配置无效", "database.open.validate"));
    options.database_path =
        std::filesystem::absolute(options.database_path, error).lexically_normal();
    if (error)
        return Result<std::unique_ptr<EventMetadataDatabase>>::failure(
            config_error("SQLite 数据库路径无效", "database.open.validate"));
    options.event_root = std::filesystem::absolute(options.event_root, error).lexically_normal();
    if (error)
        return Result<std::unique_ptr<EventMetadataDatabase>>::failure(
            config_error("事件根目录路径无效", "database.open.validate"));
    if (options.backup_directory.empty())
        options.backup_directory = options.database_path.parent_path() / "backups";
    options.backup_directory =
        std::filesystem::absolute(options.backup_directory, error).lexically_normal();
    if (error)
        return Result<std::unique_ptr<EventMetadataDatabase>>::failure(
            config_error("数据库备份路径无效", "database.open.validate"));

    const bool existed = std::filesystem::exists(options.database_path, error);
    if (error)
        return Result<std::unique_ptr<EventMetadataDatabase>>::failure(
            config_error("无法检查 SQLite 数据库路径", "database.open.validate"));
    std::filesystem::create_directories(options.database_path.parent_path(), error);
    if (error)
        return Result<std::unique_ptr<EventMetadataDatabase>>::failure(
            config_error("无法创建 SQLite 数据库父目录", "database.open.directory"));
    std::filesystem::create_directories(options.event_root, error);
    if (error)
        return Result<std::unique_ptr<EventMetadataDatabase>>::failure(
            config_error("无法创建事件根目录", "database.open.directory"));

    auto opened = open_connection(
        options.database_path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        static_cast<int>(options.busy_timeout.count()));
    if (!opened)
        return Result<std::unique_ptr<EventMetadataDatabase>>::failure(std::move(opened).error());
    auto connection = std::move(opened).value();
    auto foreign_keys = execute(connection.get(), "PRAGMA foreign_keys=ON", "database.configure");
    if (!foreign_keys)
        return Result<std::unique_ptr<EventMetadataDatabase>>::failure(
            std::move(foreign_keys).error());
    auto checked = quick_check(connection.get());
    if (!checked)
        return Result<std::unique_ptr<EventMetadataDatabase>>::failure(std::move(checked).error());
    auto version = read_schema_version(connection.get());
    if (!version)
        return Result<std::unique_ptr<EventMetadataDatabase>>::failure(std::move(version).error());
    if (version.value() > database_schema_version)
    {
        Error unsupported =
            make_error("DATABASE_SCHEMA_UNSUPPORTED", Severity::critical,
                       "数据库 schema 高于当前程序支持版本", "storage", "database.open.version");
        unsupported.details.push_back(
            {.key = "databaseVersion", .value = std::to_string(version.value())});
        unsupported.details.push_back(
            {.key = "supportedVersion", .value = std::to_string(database_schema_version)});
        return Result<std::unique_ptr<EventMetadataDatabase>>::failure(std::move(unsupported));
    }

    MetadataDatabaseOpenReport report{.schema_version = version.value(), .created = !existed};
    if (version.value() < database_schema_version)
    {
        if (existed)
        {
            std::filesystem::create_directories(options.backup_directory, error);
            if (error)
                return Result<std::unique_ptr<EventMetadataDatabase>>::failure(
                    config_error("无法创建迁移备份目录", "database.migrate.backup"));
            std::filesystem::path backup_path;
            for (std::size_t attempt = 0U; attempt < 1024U; ++attempt)
            {
                const auto suffix = attempt == 0U ? std::string{} : "." + std::to_string(attempt);
                backup_path = options.backup_directory /
                              (options.database_path.filename().string() + ".pre-migration-v" +
                               std::to_string(version.value()) + suffix + ".bak");
                if (!std::filesystem::exists(backup_path, error) && !error)
                    break;
                backup_path.clear();
                error.clear();
            }
            if (backup_path.empty())
                return Result<std::unique_ptr<EventMetadataDatabase>>::failure(
                    database_error(connection.get(), SQLITE_FULL, "database.migrate.backup",
                                   "无法分配唯一迁移备份路径", true));
            auto backup = backup_connection(connection.get(), backup_path,
                                            static_cast<int>(options.busy_timeout.count()));
            if (!backup)
                return Result<std::unique_ptr<EventMetadataDatabase>>::failure(
                    std::move(backup).error());
            report.migration_backup = backup_path;
        }
        auto migrated = Result<void>::success();
        if (version.value() == 0U)
            migrated = migrate_to_v1(connection.get());
        if (migrated && version.value() <= 1U)
            migrated = migrate_to_v2(connection.get());
        if (!migrated)
            return Result<std::unique_ptr<EventMetadataDatabase>>::failure(
                std::move(migrated).error());
        report.migrated = true;
        report.schema_version = database_schema_version;
    }
    checked = quick_check(connection.get());
    if (!checked)
        return Result<std::unique_ptr<EventMetadataDatabase>>::failure(std::move(checked).error());
    auto impl = std::make_unique<MetadataDatabaseImpl>();
    impl->options = std::move(options);
    impl->database = std::move(connection);
    impl->report = std::move(report);
    return Result<std::unique_ptr<EventMetadataDatabase>>::success(
        std::make_unique<EventMetadataDatabase>(ConstructionKey{}, std::move(impl)));
}

EventMetadataDatabase::EventMetadataDatabase(ConstructionKey,
                                             std::unique_ptr<MetadataDatabaseImpl> impl)
    : impl_(std::move(impl))
{
}

EventMetadataDatabase::~EventMetadataDatabase() = default;

const MetadataDatabaseOpenReport& EventMetadataDatabase::open_report() const noexcept
{
    return impl_->report;
}

Result<DatabaseIntegrityReport> EventMetadataDatabase::integrity_check() const
{
    const std::scoped_lock lock{impl_->mutex};
    return quick_check(impl_->database.get());
}

Result<void> EventMetadataDatabase::backup_to(const std::filesystem::path& destination) const
{
    std::error_code error;
    auto absolute = std::filesystem::absolute(destination, error).lexically_normal();
    if (error || destination.empty() || absolute == impl_->options.database_path)
        return Result<void>::failure(
            config_error("数据库备份目标无效", "database.backup.validate"));
    std::filesystem::create_directories(absolute.parent_path(), error);
    if (error)
        return Result<void>::failure(
            config_error("无法创建数据库备份目录", "database.backup.directory"));
    const std::scoped_lock lock{impl_->mutex};
    return backup_connection(impl_->database.get(), absolute,
                             static_cast<int>(impl_->options.busy_timeout.count()));
}

Result<void> EventMetadataDatabase::restore_backup(const std::filesystem::path& database_path,
                                                   const std::filesystem::path& backup_path,
                                                   const std::chrono::milliseconds busy_timeout)
{
    std::error_code path_error;
    auto target = std::filesystem::absolute(database_path, path_error).lexically_normal();
    if (path_error || database_path.empty() || backup_path.empty() || busy_timeout.count() <= 0 ||
        busy_timeout.count() > 60000)
        return Result<void>::failure(
            config_error("数据库恢复参数无效", "database.restore.validate"));
    auto source_path = std::filesystem::absolute(backup_path, path_error).lexically_normal();
    if (path_error || target == source_path || !std::filesystem::is_regular_file(source_path))
        return Result<void>::failure(config_error("数据库恢复源无效", "database.restore.validate"));
    auto source = open_connection(source_path, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
                                  static_cast<int>(busy_timeout.count()));
    if (!source)
        return Result<void>::failure(std::move(source).error());
    auto source_connection = std::move(source).value();
    auto checked = quick_check(source_connection.get());
    if (!checked)
        return Result<void>::failure(std::move(checked).error());
    std::filesystem::create_directories(target.parent_path(), path_error);
    if (path_error)
        return Result<void>::failure(
            config_error("无法创建数据库恢复目录", "database.restore.directory"));

    std::filesystem::path temporary;
    for (std::size_t attempt = 0U; attempt < 1024U; ++attempt)
    {
        temporary = target.parent_path() /
                    (target.filename().string() + ".restore." + std::to_string(attempt) + ".tmp");
        if (!std::filesystem::exists(temporary, path_error) && !path_error)
            break;
        temporary.clear();
        path_error.clear();
    }
    if (temporary.empty())
        return Result<void>::failure(database_error(source_connection.get(), SQLITE_FULL,
                                                    "database.restore.temporary",
                                                    "无法分配唯一数据库恢复临时文件"));
    auto copied = backup_connection(source_connection.get(), temporary,
                                    static_cast<int>(busy_timeout.count()));
    if (!copied)
        return copied;
    for (const auto& sidecar : {std::filesystem::path{target.wstring() + L"-wal"},
                                std::filesystem::path{target.wstring() + L"-shm"},
                                std::filesystem::path{target.wstring() + L"-journal"}})
    {
        if (DeleteFileW(sidecar.c_str()) == 0)
        {
            const auto native = GetLastError();
            if (native != ERROR_FILE_NOT_FOUND && native != ERROR_PATH_NOT_FOUND)
            {
                Error error = make_error("DATABASE_ERROR", Severity::critical,
                                         "数据库恢复前无法清理已关闭目标的 SQLite 边车文件",
                                         "storage", "database.restore.sidecar");
                error.native_domain = "win32";
                error.native_code = std::to_string(native);
                error.details.push_back({.key = "path", .value = sidecar.string()});
                return Result<void>::failure(std::move(error));
            }
        }
    }
    const bool target_exists = std::filesystem::exists(target, path_error) && !path_error;
    const auto replaced =
        target_exists ? ReplaceFileW(target.c_str(), temporary.c_str(), nullptr,
                                     REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)
                      : MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH);
    if (replaced == 0)
    {
        Error error =
            make_error("DATABASE_ERROR", Severity::critical, "数据库备份无法原子恢复到目标",
                       "storage", "database.restore.commit");
        error.native_domain = "win32";
        error.native_code = std::to_string(GetLastError());
        error.details.push_back({.key = "temporary", .value = temporary.string()});
        return Result<void>::failure(std::move(error));
    }
    auto restored = open_connection(target, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
                                    static_cast<int>(busy_timeout.count()));
    if (!restored)
        return Result<void>::failure(std::move(restored).error());
    auto restored_connection = std::move(restored).value();
    checked = quick_check(restored_connection.get());
    if (!checked)
        return Result<void>::failure(std::move(checked).error());
    return Result<void>::success();
}

Result<void> EventMetadataDatabase::index_committed_event(
    const std::filesystem::path& committed_directory)
{
    auto writer = EventTransactionWriter::create(
        {.event_root = impl_->options.event_root,
         .maximum_manifest_bytes = impl_->options.maximum_manifest_bytes,
         .maximum_recovery_entries =
             std::min<std::size_t>(impl_->options.maximum_reconcile_events, 4096U)});
    if (!writer)
        return Result<void>::failure(std::move(writer).error());
    auto manifest = writer.value()->verify_committed_manifest(committed_directory);
    if (!manifest)
    {
        auto error = reconcile_error("正式事件目录未通过 manifest 校验", "database.index.verify",
                                     committed_directory);
        error.details.push_back({.key = "cause", .value = manifest.error().business_code});
        return Result<void>::failure(std::move(error));
    }
    auto indexed = parse_index_manifest(manifest.value());
    if (!indexed)
        return Result<void>::failure(std::move(indexed).error());
    const std::scoped_lock lock{impl_->mutex};
    auto begun = execute(impl_->database.get(), "BEGIN IMMEDIATE", "database.index.begin");
    if (!begun)
        return begun;
    Result<void> result = Result<void>::success();
    try
    {
        result = index_manifest(impl_->database.get(), indexed.value());
    }
    catch (const std::exception&)
    {
        result =
            Result<void>::failure(reconcile_error("事件 manifest 子项无法安全转换为数据库索引",
                                                  "database.index.convert", committed_directory));
    }
    if (result)
        result = execute(impl_->database.get(), "COMMIT", "database.index.commit");
    if (!result)
    {
        static_cast<void>(execute(impl_->database.get(), "ROLLBACK", "database.index.rollback"));
        return result;
    }
    return Result<void>::success();
}

Result<EventQueryPage> EventMetadataDatabase::query_events(const EventQuery& query) const
{
    if (query.limit == 0U || query.limit > database_maximum_page_size ||
        query.offset > maximum_query_offset ||
        (query.start_time_utc_ms && query.end_time_utc_ms &&
         *query.start_time_utc_ms > *query.end_time_utc_ms) ||
        (query.event_state && !valid_text(*query.event_state)) ||
        (query.camera_id && !valid_text(*query.camera_id)))
        return Result<EventQueryPage>::failure(
            config_error("事件分页或筛选参数无效", "database.query.validate"));

    std::string where = " WHERE 1=1";
    if (query.start_time_utc_ms)
        where += " AND e.candidate_time_utc_ms>=?";
    if (query.end_time_utc_ms)
        where += " AND e.candidate_time_utc_ms<=?";
    if (query.event_state)
        where += " AND e.event_state=?";
    if (query.camera_id)
        where += " AND EXISTS(SELECT 1 FROM event_cameras c WHERE c.event_id=e.event_id AND "
                 "c.camera_id=?)";
    const auto bind_filters = [&](sqlite3_stmt* statement) {
        int index = 1;
        bool bound = true;
        if (query.start_time_utc_ms)
            bound = bound && bind_int64(statement, index, *query.start_time_utc_ms);
        if (query.end_time_utc_ms)
            bound = bound && bind_int64(statement, index, *query.end_time_utc_ms);
        if (query.event_state)
            bound = bound && bind_text(statement, index, *query.event_state);
        if (query.camera_id)
            bound = bound && bind_text(statement, index, *query.camera_id);
        return std::pair{bound, index};
    };

    const std::scoped_lock lock{impl_->mutex};
    auto count = prepare(impl_->database.get(), "SELECT COUNT(*) FROM events e" + where,
                         "database.query.count");
    if (!count)
        return Result<EventQueryPage>::failure(std::move(count).error());
    auto count_statement = std::move(count).value();
    if (!bind_filters(count_statement.get()).first)
        return Result<EventQueryPage>::failure(
            std::move(bind_failure(impl_->database.get(), "database.query.count.bind")).error());
    auto result = sqlite3_step(count_statement.get());
    if (result != SQLITE_ROW)
        return Result<EventQueryPage>::failure(database_error(
            impl_->database.get(), result, "database.query.count", "无法统计事件查询结果"));
    const auto total_value = sqlite3_column_int64(count_statement.get(), 0);
    if (total_value < 0)
        return Result<EventQueryPage>::failure(database_error(
            impl_->database.get(), SQLITE_CORRUPT, "database.query.count", "事件计数无效"));

    const std::string select =
        R"sql(
SELECT e.event_id,e.event_schema_version,e.event_state,e.candidate_time_utc_ms,
 e.confirmed_time_utc_ms,e.start_time_utc_ms,e.end_time_utc_ms,e.trigger_camera_id,
 e.trigger_frame_number,e.trigger_reason,e.confidence,e.upload_state,e.storage_state,
 r.locked,r.deletion_allowed,r.deletion_state,e.relative_directory
 FROM events e JOIN event_retention r ON r.event_id=e.event_id)sql" +
        where + " ORDER BY e.candidate_time_utc_ms DESC,e.event_id DESC LIMIT ? OFFSET ?";
    auto rows = prepare(impl_->database.get(), select, "database.query.rows");
    if (!rows)
        return Result<EventQueryPage>::failure(std::move(rows).error());
    auto row_statement = std::move(rows).value();
    auto [bound, index] = bind_filters(row_statement.get());
    bound = bound &&
            bind_int64(row_statement.get(), index, static_cast<std::int64_t>(query.limit)) &&
            bind_int64(row_statement.get(), index, static_cast<std::int64_t>(query.offset));
    if (!bound)
        return Result<EventQueryPage>::failure(
            std::move(bind_failure(impl_->database.get(), "database.query.rows.bind")).error());

    EventQueryPage page{.total = static_cast<std::size_t>(total_value),
                        .offset = query.offset,
                        .limit = query.limit};
    while ((result = sqlite3_step(row_statement.get())) == SQLITE_ROW)
    {
        const auto frame_number = sqlite3_column_int64(row_statement.get(), 8);
        if (frame_number < 0)
            return Result<EventQueryPage>::failure(database_error(
                impl_->database.get(), SQLITE_CORRUPT, "database.query.rows", "事件帧号无效"));
        EventMetadataRecord event{
            .event_id = column_text(row_statement.get(), 0),
            .event_schema_version =
                static_cast<std::uint32_t>(sqlite3_column_int64(row_statement.get(), 1)),
            .event_state = column_text(row_statement.get(), 2),
            .candidate_time_utc_ms = sqlite3_column_int64(row_statement.get(), 3),
            .start_time_utc_ms = sqlite3_column_int64(row_statement.get(), 5),
            .end_time_utc_ms = sqlite3_column_int64(row_statement.get(), 6),
            .trigger_camera_id = column_text(row_statement.get(), 7),
            .trigger_frame_number = static_cast<std::uint64_t>(frame_number),
            .trigger_reason = column_text(row_statement.get(), 9),
            .confidence = sqlite3_column_double(row_statement.get(), 10),
            .upload_state = column_text(row_statement.get(), 11),
            .storage_state = column_text(row_statement.get(), 12),
            .retention_locked = sqlite3_column_int64(row_statement.get(), 13) != 0,
            .deletion_allowed = sqlite3_column_int64(row_statement.get(), 14) != 0,
            .deletion_state = column_text(row_statement.get(), 15),
            .relative_directory = column_text(row_statement.get(), 16)};
        if (sqlite3_column_type(row_statement.get(), 4) != SQLITE_NULL)
            event.confirmed_time_utc_ms = sqlite3_column_int64(row_statement.get(), 4);
        page.events.push_back(std::move(event));
    }
    if (result != SQLITE_DONE)
        return Result<EventQueryPage>::failure(database_error(
            impl_->database.get(), result, "database.query.rows", "事件分页查询失败"));

    auto cameras =
        prepare(impl_->database.get(),
                "SELECT camera_id FROM event_cameras WHERE event_id=? ORDER BY camera_id",
                "database.query.cameras");
    if (!cameras)
        return Result<EventQueryPage>::failure(std::move(cameras).error());
    auto camera_statement = std::move(cameras).value();
    for (auto& event : page.events)
    {
        sqlite3_reset(camera_statement.get());
        sqlite3_clear_bindings(camera_statement.get());
        int camera_index = 1;
        if (!bind_text(camera_statement.get(), camera_index, event.event_id))
            return Result<EventQueryPage>::failure(
                std::move(bind_failure(impl_->database.get(), "database.query.cameras.bind"))
                    .error());
        while ((result = sqlite3_step(camera_statement.get())) == SQLITE_ROW)
            event.camera_ids.push_back(column_text(camera_statement.get(), 0));
        if (result != SQLITE_DONE)
            return Result<EventQueryPage>::failure(database_error(impl_->database.get(), result,
                                                                  "database.query.cameras",
                                                                  "事件相机筛选结果读取失败"));
    }
    return Result<EventQueryPage>::success(std::move(page));
}

Result<void> EventMetadataDatabase::set_retention_policy(const std::string_view event_id,
                                                         const bool locked,
                                                         const bool deletion_allowed,
                                                         const std::int64_t updated_at_utc_ms)
{
    if (!valid_text(event_id) || updated_at_utc_ms < 0)
        return Result<void>::failure(
            config_error("事件保留策略参数无效", "database.retention.set.validate"));
    const std::scoped_lock lock{impl_->mutex};
    auto prepared = prepare(impl_->database.get(), R"sql(
UPDATE event_retention SET locked=?,deletion_allowed=?,updated_at_utc_ms=?
 WHERE event_id=? AND deletion_state='Active'
)sql",
                            "database.retention.set");
    if (!prepared)
        return Result<void>::failure(std::move(prepared).error());
    auto statement = std::move(prepared).value();
    int index = 1;
    if (!bind_int64(statement.get(), index, locked ? 1 : 0) ||
        !bind_int64(statement.get(), index, deletion_allowed ? 1 : 0) ||
        !bind_int64(statement.get(), index, updated_at_utc_ms) ||
        !bind_text(statement.get(), index, event_id))
        return bind_failure(impl_->database.get(), "database.retention.set.bind");
    auto updated = step_done(impl_->database.get(), statement.get(), "database.retention.set");
    if (!updated)
        return updated;
    if (sqlite3_changes(impl_->database.get()) != 1)
        return Result<void>::failure(
            reconcile_error("事件不存在或已完成删除", "database.retention.set"));
    return Result<void>::success();
}

namespace
{
Result<std::vector<EventRetentionRecord>> read_retention_rows(
    sqlite3* database, const std::string_view sql,
    const std::function<bool(sqlite3_stmt*, int&)>& bind, const std::string_view operation)
{
    auto prepared = prepare(database, sql, operation);
    if (!prepared)
        return Result<std::vector<EventRetentionRecord>>::failure(std::move(prepared).error());
    auto statement = std::move(prepared).value();
    int index = 1;
    if (!bind(statement.get(), index))
        return Result<std::vector<EventRetentionRecord>>::failure(
            std::move(bind_failure(database, std::string{operation} + ".bind")).error());
    std::vector<EventRetentionRecord> rows;
    int result = SQLITE_ROW;
    while ((result = sqlite3_step(statement.get())) == SQLITE_ROW)
    {
        const auto bytes = sqlite3_column_int64(statement.get(), 5);
        if (bytes < 0)
            return Result<std::vector<EventRetentionRecord>>::failure(
                database_error(database, SQLITE_CORRUPT, operation, "事件文件占用统计为负数"));
        rows.push_back({.event_id = column_text(statement.get(), 0),
                        .candidate_time_utc_ms = sqlite3_column_int64(statement.get(), 1),
                        .upload_state = column_text(statement.get(), 2),
                        .storage_state = column_text(statement.get(), 3),
                        .relative_directory = column_text(statement.get(), 4),
                        .indexed_file_bytes = static_cast<std::uint64_t>(bytes),
                        .locked = sqlite3_column_int64(statement.get(), 6) != 0,
                        .deletion_allowed = sqlite3_column_int64(statement.get(), 7) != 0,
                        .deletion_state = column_text(statement.get(), 8),
                        .deletion_relative_path = column_text(statement.get(), 9),
                        .last_error = column_text(statement.get(), 10)});
    }
    if (result != SQLITE_DONE)
        return Result<std::vector<EventRetentionRecord>>::failure(
            database_error(database, result, operation, "事件保留记录查询失败"));
    return Result<std::vector<EventRetentionRecord>>::success(std::move(rows));
}

constexpr std::string_view retention_select = R"sql(
SELECT e.event_id,e.candidate_time_utc_ms,e.upload_state,e.storage_state,e.relative_directory,
 COALESCE((SELECT SUM(f.size_bytes) FROM event_files f WHERE f.event_id=e.event_id),0)
   + r.manifest_size_bytes,
 r.locked,r.deletion_allowed,r.deletion_state,r.deletion_relative_path,r.last_error
 FROM events e JOIN event_retention r ON r.event_id=e.event_id
)sql";
} // namespace

Result<std::vector<EventRetentionRecord>> EventMetadataDatabase::retention_candidates(
    const std::optional<std::int64_t> candidate_time_cutoff_utc_ms, const std::size_t limit) const
{
    if (limit == 0U || limit > database_maximum_page_size ||
        (candidate_time_cutoff_utc_ms && *candidate_time_cutoff_utc_ms < 0))
        return Result<std::vector<EventRetentionRecord>>::failure(
            config_error("保留清理查询参数无效", "database.retention.candidates.validate"));
    std::string sql{retention_select};
    sql += R"sql( WHERE e.upload_state='Uploaded' AND e.storage_state='Present'
 AND r.locked=0 AND r.deletion_allowed=1 AND r.deletion_state='Active')sql";
    if (candidate_time_cutoff_utc_ms)
        sql += " AND e.candidate_time_utc_ms<=?";
    sql += " ORDER BY e.candidate_time_utc_ms ASC,e.event_id ASC LIMIT ?";
    const std::scoped_lock lock{impl_->mutex};
    return read_retention_rows(
        impl_->database.get(), sql,
        [&](sqlite3_stmt* statement, int& index) {
            bool bound = true;
            if (candidate_time_cutoff_utc_ms)
                bound = bind_int64(statement, index, *candidate_time_cutoff_utc_ms);
            return bound && bind_int64(statement, index, static_cast<std::int64_t>(limit));
        },
        "database.retention.candidates");
}

Result<std::vector<EventRetentionRecord>> EventMetadataDatabase::deletion_work(
    const std::size_t limit) const
{
    if (limit == 0U || limit > database_maximum_page_size)
        return Result<std::vector<EventRetentionRecord>>::failure(
            config_error("删除恢复查询参数无效", "database.retention.work.validate"));
    std::string sql{retention_select};
    sql += R"sql( WHERE r.deletion_state IN ('DeletePending','DeleteFailed')
 ORDER BY e.candidate_time_utc_ms ASC,e.event_id ASC LIMIT ?)sql";
    const std::scoped_lock lock{impl_->mutex};
    return read_retention_rows(
        impl_->database.get(), sql,
        [&](sqlite3_stmt* statement, int& index) {
            return bind_int64(statement, index, static_cast<std::int64_t>(limit));
        },
        "database.retention.work");
}

Result<bool> EventMetadataDatabase::begin_deletion(
    const std::string_view event_id, const std::filesystem::path& deletion_relative_path,
    const std::int64_t updated_at_utc_ms)
{
    const auto relative = deletion_relative_path.generic_string();
    std::vector<std::filesystem::path> components;
    for (const auto& component : deletion_relative_path)
        components.push_back(component);
    if (!valid_text(event_id) || !valid_text(relative) || deletion_relative_path.is_absolute() ||
        components.size() != 2U || components.front() != ".deletions" ||
        components.back().generic_string() != std::string{event_id} + ".deleting" ||
        updated_at_utc_ms < 0)
        return Result<bool>::failure(
            config_error("事件删除声明参数无效", "database.retention.begin.validate"));
    const std::scoped_lock lock{impl_->mutex};
    auto prepared = prepare(impl_->database.get(), R"sql(
UPDATE event_retention SET deletion_state='DeletePending',deletion_relative_path=?,last_error='',
 updated_at_utc_ms=? WHERE event_id=? AND
 (deletion_state='DeleteFailed' OR
  (deletion_state='Active' AND locked=0 AND deletion_allowed=1
   AND EXISTS(SELECT 1 FROM events e WHERE e.event_id=event_retention.event_id
              AND e.upload_state='Uploaded' AND e.storage_state='Present')))
)sql",
                            "database.retention.begin");
    if (!prepared)
        return Result<bool>::failure(std::move(prepared).error());
    auto statement = std::move(prepared).value();
    int index = 1;
    if (!bind_text(statement.get(), index, relative) ||
        !bind_int64(statement.get(), index, updated_at_utc_ms) ||
        !bind_text(statement.get(), index, event_id))
        return Result<bool>::failure(
            std::move(bind_failure(impl_->database.get(), "database.retention.begin.bind"))
                .error());
    auto updated = step_done(impl_->database.get(), statement.get(), "database.retention.begin");
    if (!updated)
        return Result<bool>::failure(std::move(updated).error());
    return Result<bool>::success(sqlite3_changes(impl_->database.get()) == 1);
}

Result<void> EventMetadataDatabase::complete_deletion(const std::string_view event_id,
                                                      const std::int64_t deleted_at_utc_ms)
{
    if (!valid_text(event_id) || deleted_at_utc_ms < 0)
        return Result<void>::failure(
            config_error("事件删除完成参数无效", "database.retention.complete.validate"));
    const std::scoped_lock lock{impl_->mutex};
    auto begun = execute(impl_->database.get(), "BEGIN IMMEDIATE", "database.retention.complete");
    if (!begun)
        return begun;
    auto retention = prepare(impl_->database.get(), R"sql(
UPDATE event_retention SET deletion_state='Deleted',last_error='',updated_at_utc_ms=?,
 deleted_at_utc_ms=? WHERE event_id=? AND deletion_state IN ('DeletePending','DeleteFailed')
)sql",
                             "database.retention.complete");
    if (!retention)
    {
        static_cast<void>(execute(impl_->database.get(), "ROLLBACK", "database.rollback"));
        return Result<void>::failure(std::move(retention).error());
    }
    auto statement = std::move(retention).value();
    int index = 1;
    if (!bind_int64(statement.get(), index, deleted_at_utc_ms) ||
        !bind_int64(statement.get(), index, deleted_at_utc_ms) ||
        !bind_text(statement.get(), index, event_id))
    {
        static_cast<void>(execute(impl_->database.get(), "ROLLBACK", "database.rollback"));
        return bind_failure(impl_->database.get(), "database.retention.complete.bind");
    }
    auto updated = step_done(impl_->database.get(), statement.get(), "database.retention.complete");
    if (!updated || sqlite3_changes(impl_->database.get()) != 1)
    {
        static_cast<void>(execute(impl_->database.get(), "ROLLBACK", "database.rollback"));
        return updated ? Result<void>::failure(reconcile_error("事件不处于可完成删除状态",
                                                               "database.retention.complete"))
                       : updated;
    }
    auto event =
        prepare(impl_->database.get(), "UPDATE events SET storage_state='Missing' WHERE event_id=?",
                "database.retention.complete.event");
    if (!event)
    {
        static_cast<void>(execute(impl_->database.get(), "ROLLBACK", "database.rollback"));
        return Result<void>::failure(std::move(event).error());
    }
    auto event_statement = std::move(event).value();
    index = 1;
    if (!bind_text(event_statement.get(), index, event_id))
    {
        static_cast<void>(execute(impl_->database.get(), "ROLLBACK", "database.rollback"));
        return bind_failure(impl_->database.get(), "database.retention.complete.event.bind");
    }
    updated = step_done(impl_->database.get(), event_statement.get(),
                        "database.retention.complete.event");
    if (!updated)
    {
        static_cast<void>(execute(impl_->database.get(), "ROLLBACK", "database.rollback"));
        return updated;
    }
    return execute(impl_->database.get(), "COMMIT", "database.retention.complete.commit");
}

Result<void> EventMetadataDatabase::fail_deletion(const std::string_view event_id,
                                                  const std::string_view reason,
                                                  const std::int64_t updated_at_utc_ms)
{
    if (!valid_text(event_id) || !valid_text(reason) || updated_at_utc_ms < 0)
        return Result<void>::failure(
            config_error("事件删除失败参数无效", "database.retention.fail.validate"));
    const std::scoped_lock lock{impl_->mutex};
    auto prepared = prepare(impl_->database.get(), R"sql(
UPDATE event_retention SET deletion_state='DeleteFailed',last_error=?,updated_at_utc_ms=?
 WHERE event_id=? AND deletion_state IN ('DeletePending','DeleteFailed')
)sql",
                            "database.retention.fail");
    if (!prepared)
        return Result<void>::failure(std::move(prepared).error());
    auto statement = std::move(prepared).value();
    int index = 1;
    if (!bind_text(statement.get(), index, reason) ||
        !bind_int64(statement.get(), index, updated_at_utc_ms) ||
        !bind_text(statement.get(), index, event_id))
        return bind_failure(impl_->database.get(), "database.retention.fail.bind");
    auto updated = step_done(impl_->database.get(), statement.get(), "database.retention.fail");
    if (!updated)
        return updated;
    if (sqlite3_changes(impl_->database.get()) != 1)
        return Result<void>::failure(
            reconcile_error("事件不处于删除工作状态", "database.retention.fail"));
    return Result<void>::success();
}

Result<std::uint64_t> EventMetadataDatabase::retained_event_bytes() const
{
    const std::scoped_lock lock{impl_->mutex};
    auto prepared = prepare(impl_->database.get(), R"sql(
SELECT COALESCE(SUM(f.size_bytes),0)
 + COALESCE((SELECT SUM(manifest_size_bytes) FROM event_retention
             WHERE deletion_state<>'Deleted'),0)
 FROM event_files f JOIN event_retention r ON r.event_id=f.event_id
 WHERE r.deletion_state<>'Deleted'
)sql",
                            "database.retention.bytes");
    if (!prepared)
        return Result<std::uint64_t>::failure(std::move(prepared).error());
    auto statement = std::move(prepared).value();
    const auto result = sqlite3_step(statement.get());
    const auto bytes = result == SQLITE_ROW ? sqlite3_column_int64(statement.get(), 0) : -1;
    if (result != SQLITE_ROW || bytes < 0)
        return Result<std::uint64_t>::failure(database_error(
            impl_->database.get(), result, "database.retention.bytes", "无法统计保留事件占用"));
    return Result<std::uint64_t>::success(static_cast<std::uint64_t>(bytes));
}

Result<EventReconcileReport> EventMetadataDatabase::reconcile()
{
    std::vector<std::filesystem::path> directories;
    directories.reserve(std::min<std::size_t>(impl_->options.maximum_reconcile_events, 1024U));
    std::error_code error;
    const auto collect = [&](const std::filesystem::path& parent, const std::size_t length,
                             auto&& visitor) -> bool {
        std::filesystem::directory_iterator iterator{
            parent, std::filesystem::directory_options::skip_permission_denied, error};
        if (error)
            return false;
        for (const auto& entry : iterator)
        {
            if (directory_component(entry, length))
                visitor(entry.path());
            if (error)
                return false;
        }
        return true;
    };
    bool scan_ok = collect(impl_->options.event_root, 4U, [&](const auto& year) {
        if (!collect(year, 2U, [&](const auto& month) {
                if (!collect(month, 2U, [&](const auto& day) {
                        std::filesystem::directory_iterator iterator{
                            day, std::filesystem::directory_options::skip_permission_denied, error};
                        if (error)
                            return;
                        for (const auto& event : iterator)
                        {
                            std::error_code type_error;
                            if (event.is_directory(type_error) && !type_error)
                            {
                                if (directories.size() >= impl_->options.maximum_reconcile_events)
                                {
                                    error = std::make_error_code(std::errc::value_too_large);
                                    return;
                                }
                                directories.push_back(event.path());
                            }
                        }
                    }))
                    return;
            }))
            return;
    });
    if (!scan_ok || error)
        return Result<EventReconcileReport>::failure(
            reconcile_error("事件目录扫描失败或超过固定对账上限", "database.reconcile.scan",
                            impl_->options.event_root));
    std::ranges::sort(directories);

    EventReconcileReport report{.directories_scanned = directories.size()};
    for (const auto& directory : directories)
    {
        EventQuery query{.limit = 1U};
        auto before = query_events(query);
        if (!before)
            return Result<EventReconcileReport>::failure(std::move(before).error());
        const auto previous_total = before.value().total;
        auto indexed = index_committed_event(directory);
        if (!indexed)
            return Result<EventReconcileReport>::failure(std::move(indexed).error());
        auto after = query_events(query);
        if (!after)
            return Result<EventReconcileReport>::failure(std::move(after).error());
        if (after.value().total > previous_total)
            ++report.indexed;
        else
            ++report.refreshed;
    }

    const std::scoped_lock lock{impl_->mutex};
    auto rows = prepare(impl_->database.get(),
                        "SELECT event_id,relative_directory,storage_state FROM events LIMIT ?",
                        "database.reconcile.rows", true);
    if (!rows)
        return Result<EventReconcileReport>::failure(std::move(rows).error());
    auto row_statement = std::move(rows).value();
    int limit_index = 1;
    if (!bind_int64(row_statement.get(), limit_index,
                    static_cast<std::int64_t>(impl_->options.maximum_reconcile_events + 1U)))
        return Result<EventReconcileReport>::failure(
            std::move(bind_failure(impl_->database.get(), "database.reconcile.rows.bind")).error());
    struct Presence final
    {
        std::string event_id;
        std::string state;
        bool present{};
    };
    std::vector<Presence> presence;
    int result = SQLITE_OK;
    while ((result = sqlite3_step(row_statement.get())) == SQLITE_ROW)
    {
        if (presence.size() >= impl_->options.maximum_reconcile_events)
            return Result<EventReconcileReport>::failure(
                reconcile_error("数据库事件数量超过固定对账上限", "database.reconcile.rows"));
        const auto event_id = column_text(row_statement.get(), 0);
        const auto relative = std::filesystem::path{column_text(row_statement.get(), 1)};
        std::error_code exists_error;
        const bool present =
            std::filesystem::is_directory(impl_->options.event_root / relative, exists_error) &&
            !exists_error;
        presence.push_back({.event_id = event_id,
                            .state = column_text(row_statement.get(), 2),
                            .present = present});
    }
    if (result != SQLITE_DONE)
        return Result<EventReconcileReport>::failure(
            database_error(impl_->database.get(), result, "database.reconcile.rows",
                           "无法读取数据库事件索引", false, true));
    auto update = prepare(impl_->database.get(),
                          "UPDATE events SET storage_state=?,indexed_at_utc_ms=? WHERE event_id=?",
                          "database.reconcile.presence", true);
    if (!update)
        return Result<EventReconcileReport>::failure(std::move(update).error());
    auto update_statement = std::move(update).value();
    auto begun = execute(impl_->database.get(), "BEGIN IMMEDIATE", "database.reconcile.begin");
    if (!begun)
        return Result<EventReconcileReport>::failure(std::move(begun).error());
    for (const auto& item : presence)
    {
        const std::string_view desired = item.present ? "Present" : "Missing";
        if (!item.present && item.state != "Missing")
            ++report.marked_missing;
        if (item.state == desired)
            continue;
        sqlite3_reset(update_statement.get());
        sqlite3_clear_bindings(update_statement.get());
        int update_index = 1;
        if (!bind_text(update_statement.get(), update_index, desired) ||
            !bind_int64(update_statement.get(), update_index, current_epoch_milliseconds()) ||
            !bind_text(update_statement.get(), update_index, item.event_id))
        {
            static_cast<void>(
                execute(impl_->database.get(), "ROLLBACK", "database.reconcile.rollback"));
            return Result<EventReconcileReport>::failure(
                std::move(bind_failure(impl_->database.get(), "database.reconcile.presence.bind"))
                    .error());
        }
        auto updated = step_done(impl_->database.get(), update_statement.get(),
                                 "database.reconcile.presence", true);
        if (!updated)
        {
            static_cast<void>(
                execute(impl_->database.get(), "ROLLBACK", "database.reconcile.rollback"));
            return Result<EventReconcileReport>::failure(std::move(updated).error());
        }
    }
    auto committed = execute(impl_->database.get(), "COMMIT", "database.reconcile.commit");
    if (!committed)
    {
        static_cast<void>(
            execute(impl_->database.get(), "ROLLBACK", "database.reconcile.rollback"));
        return Result<EventReconcileReport>::failure(std::move(committed).error());
    }
    return Result<EventReconcileReport>::success(std::move(report));
}

} // namespace paperbreak::storage
