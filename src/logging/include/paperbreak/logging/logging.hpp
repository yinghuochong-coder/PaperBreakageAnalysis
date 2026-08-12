#pragma once

#include "paperbreak/common/result.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace paperbreak::logging
{

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

enum class Level
{
    trace,
    debug,
    info,
    warning,
    error,
    critical,
};

struct LoggingConfig final
{
    std::filesystem::path directory;
    /// Process prefix, normally paperbreak-service or paperbreak-console.
    std::string file_stem{"paperbreak-service"};
    std::size_t max_file_size_bytes{10U * 1024U * 1024U};
    std::size_t max_files_per_day{5U};
    std::size_t queue_capacity{8192U};
    std::size_t recent_record_capacity{2048U};
    std::size_t maximum_thread_file_states{64U};
    std::uint32_t retention_days{30U};
    Level minimum_level{Level::info};
    /// Bootstrap enables this for user-facing processes; library tests may keep it disabled.
    bool console_output_enabled{};
};

struct StructuredField final
{
    std::string_view key;
    std::string_view value;
};

struct StructuredLog final
{
    Category category{Category::service};
    Level level{Level::info};
    std::string_view operation;
    std::string_view result;
    std::string_view business_code;
    std::string_view correlation_id;
    std::span<const StructuredField> fields;
};

struct RecentLogRecord final
{
    std::uint64_t sequence{};
    std::string timestamp;
    std::uint64_t thread_id{};
    std::string thread_name;
    Category category{Category::service};
    Level level{Level::info};
    std::string message;
};

struct RecentLogQuery final
{
    std::optional<std::uint64_t> after_sequence;
    std::vector<Category> categories;
    std::optional<Level> minimum_level;
    std::optional<std::string> thread_name;
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

class LoggingRuntime final
{
  private:
    struct State;

  public:
    class ThreadRegistration final
    {
      public:
        ThreadRegistration() = default;
        ~ThreadRegistration();
        ThreadRegistration(const ThreadRegistration&) = delete;
        ThreadRegistration& operator=(const ThreadRegistration&) = delete;
        ThreadRegistration(ThreadRegistration&& other) noexcept;
        ThreadRegistration& operator=(ThreadRegistration&& other) noexcept;

        [[nodiscard]] std::string_view name() const noexcept;
        [[nodiscard]] explicit operator bool() const noexcept;

      private:
        friend class LoggingRuntime;
        ThreadRegistration(std::weak_ptr<State> state, std::uint64_t thread_id, std::string name);
        void reset() noexcept;

        std::weak_ptr<State> state_;
        std::uint64_t thread_id_{};
        std::string name_;
    };

    [[nodiscard]] static Result<std::unique_ptr<LoggingRuntime>> create(
        const LoggingConfig& config);

    ~LoggingRuntime();
    LoggingRuntime(const LoggingRuntime&) = delete;
    LoggingRuntime& operator=(const LoggingRuntime&) = delete;
    LoggingRuntime(LoggingRuntime&&) = delete;
    LoggingRuntime& operator=(LoggingRuntime&&) = delete;

    /// Registers a unique logical name for the calling thread and sets its Windows description.
    [[nodiscard]] Result<ThreadRegistration> register_current_thread(std::string_view name);

    [[nodiscard]] bool enabled(Level level) const noexcept;
    [[nodiscard]] Result<void> set_minimum_level(Level level) noexcept;
    [[nodiscard]] Level minimum_level() const noexcept;
    [[nodiscard]] Result<void> set_retention_days(std::uint32_t days) noexcept;
    [[nodiscard]] std::uint32_t retention_days() const noexcept;

    /// Redacts secrets and enqueues a categorized record without waiting for disk I/O.
    [[nodiscard]] Result<void> log(Category category, Level level,
                                   std::string_view message) noexcept;
    /// Enqueues a bounded structured record. At most 16 fields are accepted.
    [[nodiscard]] Result<void> log(const StructuredLog& record) noexcept;

    [[nodiscard]] Result<void> shutdown() noexcept;
    [[nodiscard]] RecentLogQueryResult tail(const RecentLogQuery& query = {}) const;
    [[nodiscard]] std::uint64_t overrun_count() const noexcept;

  private:
    explicit LoggingRuntime(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
};

[[nodiscard]] std::string_view category_name(Category category) noexcept;
[[nodiscard]] std::optional<Category> parse_category(std::string_view value) noexcept;
[[nodiscard]] std::string_view level_name(Level level) noexcept;
[[nodiscard]] std::optional<Level> parse_level(std::string_view value) noexcept;
[[nodiscard]] bool valid_thread_name(std::string_view value) noexcept;
[[nodiscard]] std::string local_rfc3339_timestamp(
    std::chrono::system_clock::time_point time = std::chrono::system_clock::now());
[[nodiscard]] std::string redact_sensitive(std::string_view input);

} // namespace paperbreak::logging
