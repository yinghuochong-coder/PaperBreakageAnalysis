#pragma once

#include "paperbreak/common/result.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

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
    Level minimum_level{Level::info};
};

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
                   std::shared_ptr<spdlog::logger> logger);

    /// Redacts secrets and enqueues a categorized record without waiting for disk I/O.
    [[nodiscard]] Result<void> log(Category category, Level level,
                                   std::string_view message) noexcept;

    /// Flushes records and joins the background worker; safe to call repeatedly.
    [[nodiscard]] Result<void> shutdown() noexcept;

  private:
    std::shared_ptr<spdlog::details::thread_pool> thread_pool_;
    std::shared_ptr<spdlog::logger> logger_;
    bool stopped_{false};
};

/// Returns the stable lowercase text for one category.
[[nodiscard]] std::string_view category_name(Category category) noexcept;

/// Replaces approved JSON and key/value secret forms with a fixed mask.
[[nodiscard]] std::string redact_sensitive(std::string_view input);

} // namespace paperbreak::logging
