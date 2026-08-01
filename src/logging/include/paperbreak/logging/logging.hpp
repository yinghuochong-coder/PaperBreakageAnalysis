#pragma once

#include "paperbreak/common/result.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace spdlog
{
class logger;
namespace details
{
class thread_pool;
}
} // namespace spdlog

namespace paperbreak::logging
{

/// Approved module categories emitted in every log record.
enum class Category
{
    service,
    camera,
    algorithm,
    event,
    storage,
    uplink,
    ipc,
    ui,
    audit,
    performance,
};

/// Log filtering and record severity levels.
enum class Level
{
    trace,
    debug,
    info,
    warning,
    error,
    critical,
};

/// Bounded asynchronous logging and file-rotation settings.
struct LoggingConfig final
{
    std::filesystem::path directory;
    std::string file_stem{"paperbreak"};
    std::size_t max_file_size_bytes{10U * 1024U * 1024U};
    std::size_t max_files_per_day{5U};
    std::size_t queue_capacity{8192U};
    std::size_t recent_record_capacity{2048U};
    Level minimum_level{Level::info};
};

struct RecentLogRecord final
{
    std::uint64_t sequence{};
    std::string timestamp;
    std::uint64_t thread_id{};
    Category category{Category::service};
    Level level{Level::info};
    std::string message;
};

struct RecentLogQuery final
{
    std::optional<std::uint64_t> after_sequence;
    std::vector<Category> categories;
    std::optional<Level> minimum_level;
    std::size_t limit{100U};
};

struct RecentLogQueryResult final
{
    std::uint64_t first_available_sequence{};
    std::uint64_t latest_sequence{};
    std::vector<RecentLogRecord> records;
    bool truncated{};
};

class RecentLogStore;

/// RAII owner of one bounded spdlog queue, worker, logger, and rolling sink.
class LoggingRuntime final
{
  private:
    struct ConstructorToken final
    {
    };

  public:
    /// Validates the configuration and starts a dedicated asynchronous logging runtime.
    [[nodiscard]] static Result<std::unique_ptr<LoggingRuntime>> create(
        const LoggingConfig& config);

    ~LoggingRuntime();

    LoggingRuntime(const LoggingRuntime&) = delete;
    LoggingRuntime& operator=(const LoggingRuntime&) = delete;
    LoggingRuntime(LoggingRuntime&&) = delete;
    LoggingRuntime& operator=(LoggingRuntime&&) = delete;

    LoggingRuntime(ConstructorToken, std::shared_ptr<spdlog::details::thread_pool> thread_pool,
                   std::shared_ptr<spdlog::logger> logger,
                   std::shared_ptr<RecentLogStore> recent_logs);

    /// Redacts secrets and enqueues a categorized record without waiting for disk I/O.
    [[nodiscard]] Result<void> log(Category category, Level level,
                                   std::string_view message) noexcept;

    /// Flushes records and joins the background worker; safe to call repeatedly.
    [[nodiscard]] Result<void> shutdown() noexcept;

    /// Returns an eventually-consistent bounded snapshot written by the logging worker.
    [[nodiscard]] RecentLogQueryResult tail(const RecentLogQuery& query = {}) const;

  private:
    mutable std::mutex state_mutex_;
    std::shared_ptr<spdlog::details::thread_pool> thread_pool_;
    std::shared_ptr<spdlog::logger> logger_;
    std::shared_ptr<RecentLogStore> recent_logs_;
    bool stopped_{false};
};

/// Returns the stable lowercase text for one category.
[[nodiscard]] std::string_view category_name(Category category) noexcept;
[[nodiscard]] std::optional<Category> parse_category(std::string_view value) noexcept;
[[nodiscard]] std::string_view level_name(Level level) noexcept;
[[nodiscard]] std::optional<Level> parse_level(std::string_view value) noexcept;

/// Replaces approved JSON and key/value secret forms with a fixed mask.
[[nodiscard]] std::string redact_sensitive(std::string_view input);

} // namespace paperbreak::logging
