#include "paperbreak/storage/nvme_index.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <charconv>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <type_traits>
#include <utility>

namespace paperbreak::storage
{
namespace
{

constexpr std::size_t maximum_text_bytes = 4096U;

Error index_error(sqlite3* database, const int native, std::string operation, std::string message,
                  std::string reason = {})
{
    auto error = make_error("NVME_INDEX_FAILED", Severity::error, std::move(message), "storage",
                            std::move(operation), true);
    error.native_domain = "sqlite";
    error.native_code =
        std::to_string(database == nullptr ? native : sqlite3_extended_errcode(database));
    if (!reason.empty())
        error.details.push_back({"reason", std::move(reason)});
    if (database != nullptr && sqlite3_errmsg(database) != nullptr)
        error.details.push_back({"sqliteMessage", sqlite3_errmsg(database)});
    return error;
}

Error validation_error(std::string operation, std::string reason)
{
    auto error = make_error("NVME_INDEX_FAILED", Severity::error, "NVMe 派生索引输入无效",
                            "storage", std::move(operation));
    error.details.push_back({"reason", std::move(reason)});
    return error;
}

std::int64_t wall_nanoseconds(const camera::WallClockTime value) noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch()).count();
}

camera::WallClockTime wall_time(const std::int64_t nanoseconds) noexcept
{
    return camera::WallClockTime{std::chrono::duration_cast<camera::WallClockTime::duration>(
        std::chrono::nanoseconds{nanoseconds})};
}

std::string block_id_text(const NvmeBlockId& id)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result(32U, '0');
    for (std::size_t index = 0U; index < id.size(); ++index)
    {
        const auto byte = std::to_integer<unsigned>(id[index]);
        result[index * 2U] = digits[(byte >> 4U) & 0x0FU];
        result[index * 2U + 1U] = digits[byte & 0x0FU];
    }
    return result;
}

std::optional<NvmeBlockId> parse_block_id(const std::string_view text) noexcept
{
    if (text.size() != 32U)
        return std::nullopt;
    const auto nibble = [](const char value) -> std::optional<unsigned> {
        if (value >= '0' && value <= '9')
            return static_cast<unsigned>(value - '0');
        if (value >= 'a' && value <= 'f')
            return static_cast<unsigned>(value - 'a' + 10);
        return std::nullopt;
    };
    NvmeBlockId result{};
    for (std::size_t index = 0U; index < result.size(); ++index)
    {
        const auto high = nibble(text[index * 2U]);
        const auto low = nibble(text[index * 2U + 1U]);
        if (!high || !low)
            return std::nullopt;
        result[index] = static_cast<std::byte>((*high << 4U) | *low);
    }
    return result;
}

std::string decimal(const std::uint64_t value)
{
    return std::to_string(value);
}

std::optional<std::uint64_t> parse_decimal(const unsigned char* text) noexcept
{
    if (text == nullptr)
        return std::nullopt;
    const auto* first = reinterpret_cast<const char*>(text);
    const auto* last = first + std::char_traits<char>::length(first);
    std::uint64_t value{};
    const auto parsed = std::from_chars(first, last, value);
    if (parsed.ec != std::errc{} || parsed.ptr != last)
        return std::nullopt;
    return value;
}

std::string utf8_path(const std::filesystem::path& path)
{
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::filesystem::path path_from_utf8(const std::string_view value)
{
    std::u8string converted;
    converted.reserve(value.size());
    for (const unsigned char byte : value)
        converted.push_back(static_cast<char8_t>(byte));
    return std::filesystem::path{converted};
}

class Connection final
{
  public:
    ~Connection()
    {
        if (value_ != nullptr)
            sqlite3_close_v2(value_);
    }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection() = default;
    [[nodiscard]] sqlite3* get() const noexcept
    {
        return value_;
    }
    [[nodiscard]] sqlite3** put() noexcept
    {
        return &value_;
    }

  private:
    sqlite3* value_{};
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
    [[nodiscard]] sqlite3_stmt* get() const noexcept
    {
        return value_;
    }

  private:
    sqlite3_stmt* value_{};
};

Result<void> execute(sqlite3* database, const std::string_view sql,
                     const std::string_view operation)
{
    char* message = nullptr;
    const auto result =
        sqlite3_exec(database, std::string{sql}.c_str(), nullptr, nullptr, &message);
    if (result == SQLITE_OK)
        return Result<void>::success();
    auto error = index_error(database, result, std::string{operation}, "SQLite 索引语句执行失败");
    if (message != nullptr)
    {
        error.details.push_back({"sqliteExecMessage", message});
        sqlite3_free(message);
    }
    return Result<void>::failure(std::move(error));
}

Result<Statement> prepare_sql(sqlite3* database, const std::string_view sql,
                              const std::string_view operation)
{
    sqlite3_stmt* statement = nullptr;
    const auto result =
        sqlite3_prepare_v2(database, sql.data(), static_cast<int>(sql.size()), &statement, nullptr);
    if (result != SQLITE_OK)
        return Result<Statement>::failure(index_error(database, result, std::string{operation},
                                                      "无法准备 NVMe 索引 SQLite 语句"));
    return Result<Statement>::success(Statement{statement});
}

Result<void> step_done(sqlite3* database, sqlite3_stmt* statement, const std::string_view operation)
{
    const auto result = sqlite3_step(statement);
    if (result == SQLITE_DONE)
        return Result<void>::success();
    return Result<void>::failure(
        index_error(database, result, std::string{operation}, "NVMe 索引写事务执行失败"));
}

bool bind_text(sqlite3_stmt* statement, int& index, const std::string_view value)
{
    return sqlite3_bind_text(statement, index++, value.data(), static_cast<int>(value.size()),
                             SQLITE_TRANSIENT) == SQLITE_OK;
}

bool bind_i64(sqlite3_stmt* statement, int& index, const std::int64_t value)
{
    return sqlite3_bind_int64(statement, index++, value) == SQLITE_OK;
}

std::string column_text(sqlite3_stmt* statement, const int index)
{
    const auto* value = sqlite3_column_text(statement, index);
    return value == nullptr ? std::string{} : reinterpret_cast<const char*>(value);
}

Result<std::int64_t> scalar_i64(sqlite3* database, const std::string_view sql,
                                const std::string_view operation)
{
    auto prepared = prepare_sql(database, sql, operation);
    if (!prepared)
        return Result<std::int64_t>::failure(std::move(prepared).error());
    auto statement = std::move(prepared).value();
    const auto result = sqlite3_step(statement.get());
    if (result != SQLITE_ROW)
        return Result<std::int64_t>::failure(
            index_error(database, result, std::string{operation}, "无法读取 NVMe 索引计数"));
    return Result<std::int64_t>::success(sqlite3_column_int64(statement.get(), 0));
}

class Transaction final
{
  public:
    explicit Transaction(sqlite3* database) : database_(database) {}
    ~Transaction()
    {
        if (active_)
            static_cast<void>(execute(database_, "ROLLBACK", "storage.nvme.index.rollback"));
    }
    [[nodiscard]] Result<void> begin()
    {
        auto result = execute(database_, "BEGIN IMMEDIATE", "storage.nvme.index.begin");
        active_ = result.has_value();
        return result;
    }
    [[nodiscard]] Result<void> commit()
    {
        auto result = execute(database_, "COMMIT", "storage.nvme.index.commit");
        if (result)
            active_ = false;
        return result;
    }

  private:
    sqlite3* database_{};
    bool active_{};
};

constexpr std::string_view schema = R"sql(
CREATE TABLE IF NOT EXISTS blocks(
  block_id TEXT PRIMARY KEY NOT NULL,
  camera_id TEXT NOT NULL,
  generation TEXT NOT NULL,
  path TEXT NOT NULL UNIQUE,
  physical_bytes INTEGER NOT NULL CHECK(physical_bytes > 0),
  start_wall_utc_ns INTEGER NOT NULL,
  end_wall_utc_ns INTEGER NOT NULL,
  start_sequence TEXT NOT NULL,
  end_sequence TEXT NOT NULL,
  frame_count INTEGER NOT NULL CHECK(frame_count > 0),
  sequence_gaps INTEGER NOT NULL CHECK(sequence_gaps >= 0),
  header_crc32c INTEGER NOT NULL,
  index_crc32c INTEGER NOT NULL,
  data_crc32c INTEGER NOT NULL,
  footer_crc32c INTEGER NOT NULL,
  commit_verified INTEGER NOT NULL CHECK(commit_verified IN (0,1))
) STRICT;
CREATE INDEX IF NOT EXISTS blocks_camera_time
  ON blocks(camera_id,start_wall_utc_ns,end_wall_utc_ns);
CREATE TABLE IF NOT EXISTS leases(
  event_id TEXT PRIMARY KEY NOT NULL,
  start_wall_utc_ns INTEGER NOT NULL,
  end_wall_utc_ns INTEGER NOT NULL
) STRICT;
CREATE TABLE IF NOT EXISTS lease_cameras(
  event_id TEXT NOT NULL REFERENCES leases(event_id) ON DELETE CASCADE,
  camera_id TEXT NOT NULL,
  PRIMARY KEY(event_id,camera_id)
) STRICT;
CREATE TABLE IF NOT EXISTS lease_blocks(
  event_id TEXT NOT NULL REFERENCES leases(event_id) ON DELETE CASCADE,
  block_id TEXT NOT NULL REFERENCES blocks(block_id) ON DELETE CASCADE,
  PRIMARY KEY(event_id,block_id)
) STRICT;
)sql";

bool valid_block(const NvmeIndexedBlock& block) noexcept
{
    const auto sequence_span = block.end_sequence_number - block.start_sequence_number;
    const bool id_present = std::ranges::any_of(
        block.block_id, [](const std::byte value) { return value != std::byte{0U}; });
    return id_present && block.generation > 0U && !block.camera_id.empty() &&
           block.camera_id.size() <= 16U && !block.path.empty() && block.physical_bytes > 0U &&
           block.frame_count > 0U && block.start_wall_clock_time <= block.end_wall_clock_time &&
           block.start_sequence_number <= block.end_sequence_number &&
           sequence_span < (std::numeric_limits<std::uint64_t>::max)() &&
           block.frame_count <= sequence_span + 1U &&
           block.sequence_gaps == sequence_span + 1U - block.frame_count &&
           (!block.start_monotonic_time || !block.end_monotonic_time ||
            *block.start_monotonic_time <= *block.end_monotonic_time);
}

} // namespace

class SqliteNvmeBlockIndex final : public INvmeBlockIndex
{
  public:
    Result<void> prepare(const std::filesystem::path& cache_root, const std::size_t maximum_blocks,
                         const std::size_t maximum_leases) override
    {
        if (cache_root.empty() || maximum_blocks == 0U || maximum_leases == 0U ||
            maximum_leases > 1024U)
            return Result<void>::failure(
                validation_error("storage.nvme.index.prepare", "invalid-limits"));
        std::scoped_lock lock{mutex_};
        if (database_.get() != nullptr)
            return Result<void>::failure(
                validation_error("storage.nvme.index.prepare", "already-prepared"));
        const auto directory = cache_root / ".index";
        std::error_code file_error;
        std::filesystem::create_directories(directory, file_error);
        if (file_error)
        {
            auto error = validation_error("storage.nvme.index.prepare", "create-directory-failed");
            error.native_domain = "filesystem";
            error.native_code = std::to_string(file_error.value());
            return Result<void>::failure(std::move(error));
        }
        const auto database_path = utf8_path(directory / "nvme-index-v1.db");
        const auto opened = sqlite3_open_v2(
            database_path.c_str(), database_.put(),
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
        if (opened != SQLITE_OK)
            return Result<void>::failure(index_error(
                database_.get(), opened, "storage.nvme.index.open", "无法打开 NVMe 派生索引"));
        sqlite3_extended_result_codes(database_.get(), 1);
        sqlite3_busy_timeout(database_.get(), 2000);
        for (const auto statement :
             {"PRAGMA foreign_keys=ON", "PRAGMA journal_mode=WAL", "PRAGMA synchronous=FULL"})
        {
            if (auto executed = execute(database_.get(), statement, "storage.nvme.index.pragma");
                !executed)
                return executed;
        }
        auto version =
            scalar_i64(database_.get(), "PRAGMA user_version", "storage.nvme.index.version");
        if (!version)
            return Result<void>::failure(std::move(version).error());
        if (version.value() != 0 &&
            version.value() != static_cast<std::int64_t>(nvme_index_schema_version))
            return Result<void>::failure(
                validation_error("storage.nvme.index.version", "unsupported-schema-version"));
        if (auto created = execute(database_.get(), schema, "storage.nvme.index.schema"); !created)
            return created;
        if (version.value() == 0)
        {
            if (auto set =
                    execute(database_.get(), "PRAGMA user_version=1", "storage.nvme.index.version");
                !set)
                return set;
        }
        auto integrity =
            prepare_sql(database_.get(), "PRAGMA quick_check(1)", "storage.nvme.index.integrity");
        if (!integrity)
            return Result<void>::failure(std::move(integrity).error());
        auto integrity_statement = std::move(integrity).value();
        if (sqlite3_step(integrity_statement.get()) != SQLITE_ROW ||
            column_text(integrity_statement.get(), 0) != "ok")
            return Result<void>::failure(index_error(database_.get(), SQLITE_CORRUPT,
                                                     "storage.nvme.index.integrity",
                                                     "NVMe 派生索引完整性检查失败"));
        auto blocks =
            scalar_i64(database_.get(), "SELECT COUNT(*) FROM blocks", "storage.nvme.index.count");
        auto leases =
            scalar_i64(database_.get(), "SELECT COUNT(*) FROM leases", "storage.nvme.index.count");
        if (!blocks || !leases)
            return Result<void>::failure(!blocks ? std::move(blocks).error()
                                                 : std::move(leases).error());
        if (blocks.value() > static_cast<std::int64_t>(maximum_blocks) ||
            leases.value() > static_cast<std::int64_t>(maximum_leases))
            return Result<void>::failure(
                validation_error("storage.nvme.index.prepare", "persisted-capacity-exceeded"));
        maximum_blocks_ = maximum_blocks;
        maximum_leases_ = maximum_leases;
        return Result<void>::success();
    }

    Result<void> register_block(NvmeIndexedBlock block) override
    {
        if (!valid_block(block) || !block.start_monotonic_time || !block.end_monotonic_time)
            return Result<void>::failure(
                validation_error("storage.nvme.index.register", "invalid-block"));
        std::scoped_lock lock{mutex_};
        if (auto ready = require_ready_locked("storage.nvme.index.register"); !ready)
            return ready;
        auto count =
            scalar_i64(database_.get(), "SELECT COUNT(*) FROM blocks", "storage.nvme.index.count");
        if (!count)
            return Result<void>::failure(std::move(count).error());
        if (count.value() >= static_cast<std::int64_t>(maximum_blocks_))
            return Result<void>::failure(
                validation_error("storage.nvme.index.register", "block-capacity"));
        Transaction transaction{database_.get()};
        if (auto begun = transaction.begin(); !begun)
            return begun;
        if (auto inserted = insert_block_locked(block); !inserted)
            return inserted;
        for (const auto& [event_id, lease] : live_leases_)
        {
            if (lease.camera_ids.contains(block.camera_id) && block.start_monotonic_time &&
                block.end_monotonic_time && *block.end_monotonic_time >= lease.start &&
                *block.start_monotonic_time <= lease.end)
            {
                if (auto attached = attach_locked(event_id, block.block_id); !attached)
                    return attached;
            }
        }
        auto committed = transaction.commit();
        if (committed)
            live_blocks_[block_id_text(block.block_id)] =
                LiveBlock{.camera_id = block.camera_id,
                          .start = *block.start_monotonic_time,
                          .end = *block.end_monotonic_time,
                          .block_id = block.block_id};
        return committed;
    }

    Result<NvmeEventLeaseOutcome> protect_event_window(NvmeEventLeaseRequest request) override
    {
        std::set<std::string> camera_ids{request.camera_ids.begin(), request.camera_ids.end()};
        if (request.event_id.empty() || request.event_id.size() > 128U || camera_ids.empty() ||
            camera_ids.size() > 4U || camera_ids.size() != request.camera_ids.size() ||
            std::ranges::any_of(camera_ids,
                                [](const auto& camera_id) {
                                    return camera_id.empty() || camera_id.size() > 16U;
                                }) ||
            request.start_monotonic_time > request.end_monotonic_time ||
            request.start_wall_clock_time > request.end_wall_clock_time)
        {
            return Result<NvmeEventLeaseOutcome>::failure(
                validation_error("storage.nvme.lease.protect", "invalid-request"));
        }
        std::scoped_lock lock{mutex_};
        if (auto ready = require_ready_locked("storage.nvme.lease.protect"); !ready)
            return Result<NvmeEventLeaseOutcome>::failure(std::move(ready).error());
        auto existing = lease_exists_locked(request.event_id);
        if (!existing)
            return Result<NvmeEventLeaseOutcome>::failure(std::move(existing).error());
        if (!existing.value())
        {
            auto count = scalar_i64(database_.get(), "SELECT COUNT(*) FROM leases",
                                    "storage.nvme.lease.count");
            if (!count)
                return Result<NvmeEventLeaseOutcome>::failure(std::move(count).error());
            if (count.value() >= static_cast<std::int64_t>(maximum_leases_))
            {
                auto error = make_error("NVME_LEASE_CAPACITY", Severity::error,
                                        "NVMe 事件租约已达到固定上限", "storage",
                                        "storage.nvme.lease.protect", true);
                error.details.push_back({"capacity", std::to_string(maximum_leases_)});
                return Result<NvmeEventLeaseOutcome>::failure(std::move(error));
            }
        }
        Transaction transaction{database_.get()};
        if (auto begun = transaction.begin(); !begun)
            return Result<NvmeEventLeaseOutcome>::failure(std::move(begun).error());
        auto upsert =
            prepare_sql(database_.get(),
                        "INSERT INTO leases(event_id,start_wall_utc_ns,end_wall_utc_ns) "
                        "VALUES(?,?,?) ON CONFLICT(event_id) DO UPDATE SET "
                        "start_wall_utc_ns=MIN(start_wall_utc_ns,excluded.start_wall_utc_ns),"
                        "end_wall_utc_ns=MAX(end_wall_utc_ns,excluded.end_wall_utc_ns)",
                        "storage.nvme.lease.upsert");
        if (!upsert)
            return Result<NvmeEventLeaseOutcome>::failure(std::move(upsert).error());
        auto upsert_statement = std::move(upsert).value();
        int parameter = 1;
        if (!bind_text(upsert_statement.get(), parameter, request.event_id) ||
            !bind_i64(upsert_statement.get(), parameter,
                      wall_nanoseconds(request.start_wall_clock_time)) ||
            !bind_i64(upsert_statement.get(), parameter,
                      wall_nanoseconds(request.end_wall_clock_time)))
            return Result<NvmeEventLeaseOutcome>::failure(
                index_error(database_.get(), SQLITE_MISUSE, "storage.nvme.lease.upsert",
                            "无法绑定 NVMe 事件租约"));
        if (auto stepped =
                step_done(database_.get(), upsert_statement.get(), "storage.nvme.lease.upsert");
            !stepped)
            return Result<NvmeEventLeaseOutcome>::failure(std::move(stepped).error());
        for (const auto& camera_id : camera_ids)
        {
            auto camera_insert =
                prepare_sql(database_.get(),
                            "INSERT OR IGNORE INTO lease_cameras(event_id,camera_id) VALUES(?,?)",
                            "storage.nvme.lease.camera");
            if (!camera_insert)
                return Result<NvmeEventLeaseOutcome>::failure(std::move(camera_insert).error());
            auto statement = std::move(camera_insert).value();
            parameter = 1;
            if (!bind_text(statement.get(), parameter, request.event_id) ||
                !bind_text(statement.get(), parameter, camera_id))
                return Result<NvmeEventLeaseOutcome>::failure(
                    index_error(database_.get(), SQLITE_MISUSE, "storage.nvme.lease.camera",
                                "无法绑定 NVMe 租约相机"));
            if (auto stepped =
                    step_done(database_.get(), statement.get(), "storage.nvme.lease.camera");
                !stepped)
                return Result<NvmeEventLeaseOutcome>::failure(std::move(stepped).error());
        }
        auto recovered_attach =
            prepare_sql(database_.get(),
                        "INSERT OR IGNORE INTO lease_blocks(event_id,block_id) "
                        "SELECT l.event_id,b.block_id FROM leases l "
                        "JOIN lease_cameras lc ON lc.event_id=l.event_id "
                        "JOIN blocks b ON b.camera_id=lc.camera_id "
                        "WHERE l.event_id=? AND b.end_wall_utc_ns>=l.start_wall_utc_ns "
                        "AND b.start_wall_utc_ns<=l.end_wall_utc_ns",
                        "storage.nvme.lease.attachRecovered");
        if (!recovered_attach)
            return Result<NvmeEventLeaseOutcome>::failure(std::move(recovered_attach).error());
        auto recovered_statement = std::move(recovered_attach).value();
        parameter = 1;
        if (!bind_text(recovered_statement.get(), parameter, request.event_id))
            return Result<NvmeEventLeaseOutcome>::failure(
                index_error(database_.get(), SQLITE_MISUSE, "storage.nvme.lease.attachRecovered",
                            "无法绑定恢复块租约 ID"));
        if (auto attached = step_done(database_.get(), recovered_statement.get(),
                                      "storage.nvme.lease.attachRecovered");
            !attached)
            return Result<NvmeEventLeaseOutcome>::failure(std::move(attached).error());
        auto live = live_leases_.find(request.event_id);
        auto live_start = request.start_monotonic_time;
        auto live_end = request.end_monotonic_time;
        auto live_camera_ids = camera_ids;
        if (live != live_leases_.end())
        {
            live_start = std::min(live_start, live->second.start);
            live_end = std::max(live_end, live->second.end);
            live_camera_ids.insert(live->second.camera_ids.begin(), live->second.camera_ids.end());
        }
        for (const auto& [unused, block] : live_blocks_)
        {
            static_cast<void>(unused);
            if (live_camera_ids.contains(block.camera_id) && block.end >= live_start &&
                block.start <= live_end)
            {
                if (auto attached = attach_locked(request.event_id, block.block_id); !attached)
                    return Result<NvmeEventLeaseOutcome>::failure(std::move(attached).error());
            }
        }
        if (auto committed = transaction.commit(); !committed)
            return Result<NvmeEventLeaseOutcome>::failure(std::move(committed).error());
        auto found = live_leases_.find(request.event_id);
        if (found == live_leases_.end())
        {
            live_leases_.emplace(request.event_id,
                                 LiveLease{.start = live_start,
                                           .end = live_end,
                                           .camera_ids = std::move(live_camera_ids)});
        }
        else
        {
            found->second.start = std::min(found->second.start, request.start_monotonic_time);
            found->second.end = std::max(found->second.end, request.end_monotonic_time);
            found->second.camera_ids.insert(camera_ids.begin(), camera_ids.end());
        }
        auto totals = lease_totals_locked(request.event_id);
        if (!totals)
            return Result<NvmeEventLeaseOutcome>::failure(std::move(totals).error());
        return Result<NvmeEventLeaseOutcome>::success({.event_id = std::move(request.event_id),
                                                       .protected_blocks = totals.value().first,
                                                       .protected_bytes = totals.value().second,
                                                       .updated_existing = existing.value()});
    }

    Result<void> release_event(const std::string_view event_id) override
    {
        if (event_id.empty() || event_id.size() > 128U)
            return Result<void>::failure(
                validation_error("storage.nvme.lease.release", "invalid-event-id"));
        std::scoped_lock lock{mutex_};
        if (auto ready = require_ready_locked("storage.nvme.lease.release"); !ready)
            return ready;
        auto deleted = prepare_sql(database_.get(), "DELETE FROM leases WHERE event_id=?",
                                   "storage.nvme.lease.release");
        if (!deleted)
            return Result<void>::failure(std::move(deleted).error());
        auto statement = std::move(deleted).value();
        int parameter = 1;
        if (!bind_text(statement.get(), parameter, event_id))
            return Result<void>::failure(index_error(database_.get(), SQLITE_MISUSE,
                                                     "storage.nvme.lease.release",
                                                     "无法绑定待释放租约"));
        if (auto stepped =
                step_done(database_.get(), statement.get(), "storage.nvme.lease.release");
            !stepped)
            return stepped;
        live_leases_.erase(std::string{event_id});
        return Result<void>::success();
    }

    Result<NvmeFrameSequenceTrace> trace_window(const NvmeBlockWindowQuery& query) const override
    {
        if (query.camera_id.empty() || query.camera_id.size() > 16U ||
            query.start_wall_clock_time > query.end_wall_clock_time || query.maximum_blocks == 0U ||
            query.maximum_blocks > nvme_default_maximum_query_blocks)
            return Result<NvmeFrameSequenceTrace>::failure(
                validation_error("storage.nvme.index.trace", "invalid-query"));
        std::scoped_lock lock{mutex_};
        if (auto ready = require_ready_locked("storage.nvme.index.trace"); !ready)
            return Result<NvmeFrameSequenceTrace>::failure(std::move(ready).error());
        auto selected = prepare_sql(
            database_.get(),
            "SELECT b.block_id,b.camera_id,b.generation,b.path,b.physical_bytes,"
            "b.start_wall_utc_ns,b.end_wall_utc_ns,b.start_sequence,b.end_sequence,"
            "b.frame_count,b.sequence_gaps,b.header_crc32c,b.index_crc32c,b.data_crc32c,"
            "b.footer_crc32c,b.commit_verified,COUNT(lb.event_id) "
            "FROM blocks b LEFT JOIN lease_blocks lb ON lb.block_id=b.block_id "
            "WHERE b.camera_id=? AND b.end_wall_utc_ns>=? AND b.start_wall_utc_ns<=? "
            "GROUP BY b.block_id ORDER BY b.start_wall_utc_ns,b.end_wall_utc_ns LIMIT ?",
            "storage.nvme.index.trace");
        if (!selected)
            return Result<NvmeFrameSequenceTrace>::failure(std::move(selected).error());
        auto statement = std::move(selected).value();
        int parameter = 1;
        if (!bind_text(statement.get(), parameter, query.camera_id) ||
            !bind_i64(statement.get(), parameter, wall_nanoseconds(query.start_wall_clock_time)) ||
            !bind_i64(statement.get(), parameter, wall_nanoseconds(query.end_wall_clock_time)) ||
            !bind_i64(statement.get(), parameter,
                      static_cast<std::int64_t>(query.maximum_blocks + 1U)))
            return Result<NvmeFrameSequenceTrace>::failure(
                index_error(database_.get(), SQLITE_MISUSE, "storage.nvme.index.trace",
                            "无法绑定 NVMe 时间窗查询"));
        NvmeFrameSequenceTrace trace{.camera_id = query.camera_id,
                                     .requested_start = query.start_wall_clock_time,
                                     .requested_end = query.end_wall_clock_time,
                                     .all_commits_verified = true};
        while (true)
        {
            const auto stepped = sqlite3_step(statement.get());
            if (stepped == SQLITE_DONE)
                break;
            if (stepped != SQLITE_ROW)
                return Result<NvmeFrameSequenceTrace>::failure(
                    index_error(database_.get(), stepped, "storage.nvme.index.trace",
                                "无法读取 NVMe 时间窗索引"));
            auto block = read_block(statement.get());
            if (!block)
                return Result<NvmeFrameSequenceTrace>::failure(
                    validation_error("storage.nvme.index.trace", "invalid-persisted-block"));
            trace.blocks.push_back(std::move(*block));
            if (trace.blocks.size() > query.maximum_blocks)
            {
                auto error = make_error("NVME_INDEX_QUERY_LIMIT", Severity::warning,
                                        "NVMe 时间窗查询超过固定块上限", "storage",
                                        "storage.nvme.index.trace", true);
                error.details.push_back({"maximumBlocks", std::to_string(query.maximum_blocks)});
                return Result<NvmeFrameSequenceTrace>::failure(std::move(error));
            }
        }
        for (std::size_t index = 0U; index < trace.blocks.size(); ++index)
        {
            const auto& block = trace.blocks[index];
            trace.sequence_gaps += block.sequence_gaps;
            trace.all_commits_verified = trace.all_commits_verified && block.commit_verified;
            if (index == 0U)
                continue;
            const auto previous_end = trace.blocks[index - 1U].end_sequence_number;
            if (block.start_sequence_number > previous_end &&
                block.start_sequence_number - previous_end > 1U)
                trace.sequence_gaps += block.start_sequence_number - previous_end - 1U;
            else if (block.start_sequence_number <= previous_end)
                trace.sequence_overlaps += previous_end - block.start_sequence_number + 1U;
        }
        trace.sequence_contiguous =
            !trace.blocks.empty() && trace.sequence_gaps == 0U && trace.sequence_overlaps == 0U;
        trace.all_commits_verified = !trace.blocks.empty() && trace.all_commits_verified;
        trace.time_range_covered =
            !trace.blocks.empty() &&
            trace.blocks.front().start_wall_clock_time <= query.start_wall_clock_time &&
            trace.blocks.back().end_wall_clock_time >= query.end_wall_clock_time;
        return Result<NvmeFrameSequenceTrace>::success(std::move(trace));
    }

    Result<std::optional<NvmeIndexedBlock>> oldest_reclaimable() const override
    {
        std::scoped_lock lock{mutex_};
        if (auto ready = require_ready_locked("storage.nvme.index.oldest"); !ready)
            return Result<std::optional<NvmeIndexedBlock>>::failure(std::move(ready).error());
        auto selected = prepare_sql(
            database_.get(),
            "SELECT b.block_id,b.camera_id,b.generation,b.path,b.physical_bytes,"
            "b.start_wall_utc_ns,b.end_wall_utc_ns,b.start_sequence,b.end_sequence,"
            "b.frame_count,b.sequence_gaps,b.header_crc32c,b.index_crc32c,b.data_crc32c,"
            "b.footer_crc32c,b.commit_verified,0 FROM blocks b "
            "WHERE NOT EXISTS(SELECT 1 FROM lease_blocks lb WHERE lb.block_id=b.block_id) "
            "ORDER BY b.start_wall_utc_ns,b.end_wall_utc_ns LIMIT 1",
            "storage.nvme.index.oldest");
        if (!selected)
            return Result<std::optional<NvmeIndexedBlock>>::failure(std::move(selected).error());
        auto statement = std::move(selected).value();
        const auto stepped = sqlite3_step(statement.get());
        if (stepped == SQLITE_DONE)
            return Result<std::optional<NvmeIndexedBlock>>::success(std::nullopt);
        if (stepped != SQLITE_ROW)
            return Result<std::optional<NvmeIndexedBlock>>::failure(
                index_error(database_.get(), stepped, "storage.nvme.index.oldest",
                            "无法读取最旧可回收 NVMe 块"));
        auto block = read_block(statement.get());
        if (!block)
            return Result<std::optional<NvmeIndexedBlock>>::failure(
                validation_error("storage.nvme.index.oldest", "invalid-persisted-block"));
        return Result<std::optional<NvmeIndexedBlock>>::success(std::move(block));
    }

    Result<void> erase_block(const NvmeBlockId& block_id) override
    {
        std::scoped_lock lock{mutex_};
        if (auto ready = require_ready_locked("storage.nvme.index.erase"); !ready)
            return ready;
        auto deleted = prepare_sql(database_.get(), "DELETE FROM blocks WHERE block_id=?",
                                   "storage.nvme.index.erase");
        if (!deleted)
            return Result<void>::failure(std::move(deleted).error());
        auto statement = std::move(deleted).value();
        int parameter = 1;
        const auto id = block_id_text(block_id);
        if (!bind_text(statement.get(), parameter, id))
            return Result<void>::failure(index_error(
                database_.get(), SQLITE_MISUSE, "storage.nvme.index.erase", "无法绑定待删除块 ID"));
        auto stepped = step_done(database_.get(), statement.get(), "storage.nvme.index.erase");
        if (stepped)
            live_blocks_.erase(id);
        return stepped;
    }

    Result<void> rebuild(const std::span<const NvmeIndexedBlock> blocks) override
    {
        if (std::ranges::any_of(blocks, [](const auto& block) { return !valid_block(block); }))
            return Result<void>::failure(
                validation_error("storage.nvme.index.rebuild", "invalid-block"));
        std::scoped_lock lock{mutex_};
        if (auto ready = require_ready_locked("storage.nvme.index.rebuild"); !ready)
            return ready;
        if (blocks.size() > maximum_blocks_)
            return Result<void>::failure(
                validation_error("storage.nvme.index.rebuild", "block-capacity"));
        Transaction transaction{database_.get()};
        if (auto begun = transaction.begin(); !begun)
            return begun;
        if (auto cleared =
                execute(database_.get(), "DELETE FROM blocks", "storage.nvme.index.rebuild.clear");
            !cleared)
            return cleared;
        live_blocks_.clear();
        for (const auto& block : blocks)
        {
            if (auto inserted = insert_block_locked(block); !inserted)
                return inserted;
        }
        if (auto attached = execute(database_.get(),
                                    "INSERT OR IGNORE INTO lease_blocks(event_id,block_id) "
                                    "SELECT l.event_id,b.block_id FROM leases l "
                                    "JOIN lease_cameras lc ON lc.event_id=l.event_id "
                                    "JOIN blocks b ON b.camera_id=lc.camera_id "
                                    "WHERE b.end_wall_utc_ns>=l.start_wall_utc_ns "
                                    "AND b.start_wall_utc_ns<=l.end_wall_utc_ns",
                                    "storage.nvme.index.rebuild.attach");
            !attached)
            return attached;
        return transaction.commit();
    }

    Result<NvmeBlockIndexSnapshot> snapshot() const override
    {
        std::scoped_lock lock{mutex_};
        if (auto ready = require_ready_locked("storage.nvme.index.snapshot"); !ready)
            return Result<NvmeBlockIndexSnapshot>::failure(std::move(ready).error());
        auto statement =
            prepare_sql(database_.get(),
                        "SELECT (SELECT COUNT(*) FROM blocks),(SELECT COUNT(*) FROM leases),"
                        "(SELECT COUNT(DISTINCT block_id) FROM lease_blocks),"
                        "COALESCE((SELECT SUM(b.physical_bytes) FROM blocks b WHERE EXISTS("
                        "SELECT 1 FROM lease_blocks lb WHERE lb.block_id=b.block_id)),0)",
                        "storage.nvme.index.snapshot");
        if (!statement)
            return Result<NvmeBlockIndexSnapshot>::failure(std::move(statement).error());
        auto query = std::move(statement).value();
        const auto stepped = sqlite3_step(query.get());
        if (stepped != SQLITE_ROW)
            return Result<NvmeBlockIndexSnapshot>::failure(index_error(
                database_.get(), stepped, "storage.nvme.index.snapshot", "无法读取 NVMe 索引快照"));
        return Result<NvmeBlockIndexSnapshot>::success(
            {.block_count = static_cast<std::size_t>(sqlite3_column_int64(query.get(), 0)),
             .maximum_blocks = maximum_blocks_,
             .active_leases = static_cast<std::size_t>(sqlite3_column_int64(query.get(), 1)),
             .maximum_leases = maximum_leases_,
             .protected_blocks = static_cast<std::size_t>(sqlite3_column_int64(query.get(), 2)),
             .protected_bytes = static_cast<std::uint64_t>(sqlite3_column_int64(query.get(), 3))});
    }

  private:
    struct LiveLease final
    {
        camera::MonotonicTime start;
        camera::MonotonicTime end;
        std::set<std::string> camera_ids;
    };

    struct LiveBlock final
    {
        std::string camera_id;
        camera::MonotonicTime start;
        camera::MonotonicTime end;
        NvmeBlockId block_id{};
    };

    Result<void> require_ready_locked(const std::string_view operation) const
    {
        return database_.get() == nullptr
                   ? Result<void>::failure(validation_error(std::string{operation}, "not-prepared"))
                   : Result<void>::success();
    }

    Result<void> insert_block_locked(const NvmeIndexedBlock& block)
    {
        auto inserted =
            prepare_sql(database_.get(),
                        "INSERT INTO blocks(block_id,camera_id,generation,path,physical_bytes,"
                        "start_wall_utc_ns,end_wall_utc_ns,start_sequence,end_sequence,frame_count,"
                        "sequence_gaps,header_crc32c,index_crc32c,data_crc32c,footer_crc32c,"
                        "commit_verified) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                        "storage.nvme.index.insert");
        if (!inserted)
            return Result<void>::failure(std::move(inserted).error());
        auto statement = std::move(inserted).value();
        int parameter = 1;
        const auto id = block_id_text(block.block_id);
        const auto generation = decimal(block.generation);
        const auto path = utf8_path(block.path);
        const auto start_sequence = decimal(block.start_sequence_number);
        const auto end_sequence = decimal(block.end_sequence_number);
        const bool bound =
            bind_text(statement.get(), parameter, id) &&
            bind_text(statement.get(), parameter, block.camera_id) &&
            bind_text(statement.get(), parameter, generation) &&
            bind_text(statement.get(), parameter, path) &&
            block.physical_bytes <=
                static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) &&
            bind_i64(statement.get(), parameter, static_cast<std::int64_t>(block.physical_bytes)) &&
            bind_i64(statement.get(), parameter, wall_nanoseconds(block.start_wall_clock_time)) &&
            bind_i64(statement.get(), parameter, wall_nanoseconds(block.end_wall_clock_time)) &&
            bind_text(statement.get(), parameter, start_sequence) &&
            bind_text(statement.get(), parameter, end_sequence) &&
            bind_i64(statement.get(), parameter, block.frame_count) &&
            block.sequence_gaps <=
                static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) &&
            bind_i64(statement.get(), parameter, static_cast<std::int64_t>(block.sequence_gaps)) &&
            bind_i64(statement.get(), parameter, block.header_crc32c) &&
            bind_i64(statement.get(), parameter, block.index_crc32c) &&
            bind_i64(statement.get(), parameter, block.data_crc32c) &&
            bind_i64(statement.get(), parameter, block.footer_crc32c) &&
            bind_i64(statement.get(), parameter, block.commit_verified ? 1 : 0);
        if (!bound)
            return Result<void>::failure(index_error(database_.get(), SQLITE_MISUSE,
                                                     "storage.nvme.index.insert",
                                                     "无法绑定 NVMe 块摘要"));
        return step_done(database_.get(), statement.get(), "storage.nvme.index.insert");
    }

    Result<void> attach_locked(const std::string_view event_id, const NvmeBlockId& block_id)
    {
        auto attached = prepare_sql(
            database_.get(), "INSERT OR IGNORE INTO lease_blocks(event_id,block_id) VALUES(?,?)",
            "storage.nvme.lease.attach");
        if (!attached)
            return Result<void>::failure(std::move(attached).error());
        auto statement = std::move(attached).value();
        int parameter = 1;
        const auto id = block_id_text(block_id);
        if (!bind_text(statement.get(), parameter, event_id) ||
            !bind_text(statement.get(), parameter, id))
            return Result<void>::failure(index_error(database_.get(), SQLITE_MISUSE,
                                                     "storage.nvme.lease.attach",
                                                     "无法绑定 NVMe 块租约引用"));
        return step_done(database_.get(), statement.get(), "storage.nvme.lease.attach");
    }

    Result<bool> lease_exists_locked(const std::string_view event_id) const
    {
        auto selected = prepare_sql(database_.get(), "SELECT 1 FROM leases WHERE event_id=?",
                                    "storage.nvme.lease.exists");
        if (!selected)
            return Result<bool>::failure(std::move(selected).error());
        auto statement = std::move(selected).value();
        int parameter = 1;
        if (!bind_text(statement.get(), parameter, event_id))
            return Result<bool>::failure(index_error(database_.get(), SQLITE_MISUSE,
                                                     "storage.nvme.lease.exists",
                                                     "无法绑定 NVMe 租约 ID"));
        const auto stepped = sqlite3_step(statement.get());
        if (stepped == SQLITE_ROW)
            return Result<bool>::success(true);
        if (stepped == SQLITE_DONE)
            return Result<bool>::success(false);
        return Result<bool>::failure(index_error(
            database_.get(), stepped, "storage.nvme.lease.exists", "无法查询 NVMe 租约"));
    }

    Result<std::pair<std::size_t, std::uint64_t>> lease_totals_locked(
        const std::string_view event_id) const
    {
        auto selected =
            prepare_sql(database_.get(),
                        "SELECT COUNT(*),COALESCE(SUM(b.physical_bytes),0) FROM lease_blocks lb "
                        "JOIN blocks b ON b.block_id=lb.block_id WHERE lb.event_id=?",
                        "storage.nvme.lease.totals");
        if (!selected)
            return Result<std::pair<std::size_t, std::uint64_t>>::failure(
                std::move(selected).error());
        auto statement = std::move(selected).value();
        int parameter = 1;
        if (!bind_text(statement.get(), parameter, event_id))
            return Result<std::pair<std::size_t, std::uint64_t>>::failure(
                index_error(database_.get(), SQLITE_MISUSE, "storage.nvme.lease.totals",
                            "无法绑定 NVMe 租约统计"));
        const auto stepped = sqlite3_step(statement.get());
        if (stepped != SQLITE_ROW)
            return Result<std::pair<std::size_t, std::uint64_t>>::failure(index_error(
                database_.get(), stepped, "storage.nvme.lease.totals", "无法读取 NVMe 租约统计"));
        return Result<std::pair<std::size_t, std::uint64_t>>::success(
            {static_cast<std::size_t>(sqlite3_column_int64(statement.get(), 0)),
             static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 1))});
    }

    static std::optional<NvmeIndexedBlock> read_block(sqlite3_stmt* statement)
    {
        const auto id_text = column_text(statement, 0);
        const auto id = parse_block_id(id_text);
        const auto generation = parse_decimal(sqlite3_column_text(statement, 2));
        const auto start_sequence = parse_decimal(sqlite3_column_text(statement, 7));
        const auto end_sequence = parse_decimal(sqlite3_column_text(statement, 8));
        if (!id || !generation || !start_sequence || !end_sequence)
            return std::nullopt;
        const auto physical = sqlite3_column_int64(statement, 4);
        const auto frame_count = sqlite3_column_int64(statement, 9);
        const auto gaps = sqlite3_column_int64(statement, 10);
        const auto lease_count = sqlite3_column_int64(statement, 16);
        if (physical <= 0 || frame_count <= 0 || gaps < 0 || lease_count < 0)
            return std::nullopt;
        return NvmeIndexedBlock{
            .block_id = *id,
            .camera_id = column_text(statement, 1),
            .generation = *generation,
            .path = path_from_utf8(column_text(statement, 3)),
            .physical_bytes = static_cast<std::uint64_t>(physical),
            .start_wall_clock_time = wall_time(sqlite3_column_int64(statement, 5)),
            .end_wall_clock_time = wall_time(sqlite3_column_int64(statement, 6)),
            .start_sequence_number = *start_sequence,
            .end_sequence_number = *end_sequence,
            .frame_count = static_cast<std::uint32_t>(frame_count),
            .sequence_gaps = static_cast<std::uint64_t>(gaps),
            .header_crc32c = static_cast<std::uint32_t>(sqlite3_column_int64(statement, 11)),
            .index_crc32c = static_cast<std::uint32_t>(sqlite3_column_int64(statement, 12)),
            .data_crc32c = static_cast<std::uint32_t>(sqlite3_column_int64(statement, 13)),
            .footer_crc32c = static_cast<std::uint32_t>(sqlite3_column_int64(statement, 14)),
            .commit_verified = sqlite3_column_int64(statement, 15) != 0,
            .lease_count = static_cast<std::size_t>(lease_count)};
    }

    mutable std::mutex mutex_;
    Connection database_;
    std::size_t maximum_blocks_{};
    std::size_t maximum_leases_{};
    std::map<std::string, LiveLease> live_leases_;
    std::map<std::string, LiveBlock> live_blocks_;
};

std::shared_ptr<INvmeBlockIndex> make_sqlite_nvme_block_index()
{
    return std::make_shared<SqliteNvmeBlockIndex>();
}

} // namespace paperbreak::storage
