#include "paperbreak/logging/logging.hpp"

#include <spdlog/async.h>
#include <spdlog/details/file_helper.h>
#include <spdlog/details/thread_pool.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/sink.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <mutex>
#include <regex>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

namespace paperbreak::logging
{
class RecentLogStore final
{
  public:
    explicit RecentLogStore(const std::size_t capacity) : capacity_(capacity) {}

    void append(RecentLogRecord record)
    {
        std::scoped_lock lock{mutex_};
        record.sequence = next_sequence_++;
        if (records_.size() >= capacity_)
        {
            records_.pop_front();
        }
        records_.push_back(std::move(record));
    }

    [[nodiscard]] RecentLogQueryResult query(const RecentLogQuery& query) const
    {
        RecentLogQueryResult result;
        const std::size_t limit = std::min<std::size_t>(query.limit, 200U);
        std::vector<RecentLogRecord> snapshot;
        {
            std::scoped_lock lock{mutex_};
            if (!records_.empty())
            {
                result.first_available_sequence = records_.front().sequence;
                result.latest_sequence = records_.back().sequence;
            }
            snapshot.assign(records_.begin(), records_.end());
        }
        std::vector<RecentLogRecord> matches;
        matches.reserve(snapshot.size());
        for (const auto& record : snapshot)
        {
            if (query.after_sequence.has_value() && record.sequence <= query.after_sequence.value())
            {
                continue;
            }
            if (!query.categories.empty() &&
                std::find(query.categories.begin(), query.categories.end(), record.category) ==
                    query.categories.end())
            {
                continue;
            }
            if (query.minimum_level.has_value() &&
                static_cast<int>(record.level) < static_cast<int>(query.minimum_level.value()))
            {
                continue;
            }
            matches.push_back(record);
        }
        if (query.after_sequence.has_value())
        {
            result.truncated =
                (!snapshot.empty() && query.after_sequence.value() < snapshot.front().sequence &&
                 snapshot.front().sequence - query.after_sequence.value() > 1U) ||
                matches.size() > limit;
            const std::size_t count = std::min(matches.size(), limit);
            result.records.assign(matches.begin(),
                                  matches.begin() + static_cast<std::ptrdiff_t>(count));
        }
        else
        {
            result.truncated =
                (!snapshot.empty() && snapshot.front().sequence > 1U) || matches.size() > limit;
            const std::size_t offset = matches.size() > limit ? matches.size() - limit : 0U;
            result.records.assign(matches.begin() + static_cast<std::ptrdiff_t>(offset),
                                  matches.end());
        }
        return result;
    }

  private:
    std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<RecentLogRecord> records_;
    std::uint64_t next_sequence_{1U};
};

namespace
{

constexpr std::size_t maximum_recent_message_bytes = 4096U;

std::string utc_timestamp(const std::chrono::system_clock::time_point time)
{
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch());
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(milliseconds);
    const auto fraction = milliseconds - seconds;
    const std::time_t value =
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::time_point{seconds});
    std::tm utc{};
    if (gmtime_s(&utc, &value) != 0)
    {
        return "1970-01-01T00:00:00.000Z";
    }
    std::array<char, 32> buffer{};
    const int count =
        std::snprintf(buffer.data(), buffer.size(), "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
                      utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min,
                      utc.tm_sec, static_cast<long long>(fraction.count()));
    if (count <= 0 || static_cast<std::size_t>(count) >= buffer.size())
    {
        return "1970-01-01T00:00:00.000Z";
    }
    return std::string{buffer.data(), static_cast<std::size_t>(count)};
}

Level from_spdlog_level(const spdlog::level::level_enum level) noexcept
{
    if (level <= spdlog::level::trace)
        return Level::trace;
    if (level == spdlog::level::debug)
        return Level::debug;
    if (level == spdlog::level::info)
        return Level::info;
    if (level == spdlog::level::warn)
        return Level::warning;
    if (level == spdlog::level::err)
        return Level::error;
    return Level::critical;
}

class RecentLogSink final : public spdlog::sinks::base_sink<std::mutex>
{
  public:
    explicit RecentLogSink(std::shared_ptr<RecentLogStore> store) : store_(std::move(store)) {}

  protected:
    void sink_it_(const spdlog::details::log_msg& log_message) override
    {
        std::string payload{log_message.payload.data(), log_message.payload.size()};
        Category category = Category::service;
        if (payload.starts_with('['))
        {
            const auto closing = payload.find(']');
            if (closing != std::string::npos)
            {
                const auto parsed =
                    parse_category(std::string_view{payload}.substr(1U, closing - 1U));
                if (parsed.has_value())
                {
                    category = parsed.value();
                }
                if (closing + 2U <= payload.size())
                {
                    payload.erase(0U, closing + 2U);
                }
            }
        }
        if (payload.size() > maximum_recent_message_bytes)
        {
            payload.resize(maximum_recent_message_bytes);
        }
        store_->append({.timestamp = utc_timestamp(log_message.time),
                        .thread_id = log_message.thread_id,
                        .category = category,
                        .level = from_spdlog_level(log_message.level),
                        .message = std::move(payload)});
    }

    void flush_() override {}

  private:
    std::shared_ptr<RecentLogStore> store_;
};

class DailySizeRotatingFileSink final : public spdlog::sinks::base_sink<std::mutex>
{
  public:
    DailySizeRotatingFileSink(std::filesystem::path directory, std::string file_stem,
                              const std::size_t max_file_size, const std::size_t max_files)
        : directory_(std::move(directory)), file_stem_(std::move(file_stem)),
          max_file_size_(max_file_size), max_files_(max_files)
    {
        open_for_date(date_key(std::chrono::system_clock::now()));
    }

  protected:
    void sink_it_(const spdlog::details::log_msg& message) override
    {
        const std::string message_date = date_key(message.time);
        if (message_date != current_date_)
        {
            open_for_date(message_date);
        }

        spdlog::memory_buf_t formatted;
        formatter_->format(message, formatted);
        if (file_.size() + formatted.size() > max_file_size_)
        {
            rotate_current_file();
        }
        file_.write(formatted);
    }

    void flush_() override
    {
        file_.flush();
    }

  private:
    static std::string date_key(const std::chrono::system_clock::time_point time)
    {
        const std::time_t value = std::chrono::system_clock::to_time_t(time);
        std::tm local{};
        if (localtime_s(&local, &value) != 0)
        {
            throw spdlog::spdlog_ex{"cannot convert log timestamp to local date"};
        }
        std::array<char, 11> buffer{};
        const int count = std::snprintf(buffer.data(), buffer.size(), "%04d-%02d-%02d",
                                        local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
        if (count != 10)
        {
            throw spdlog::spdlog_ex{"cannot format log date"};
        }
        return std::string{buffer.data(), 10U};
    }

    [[nodiscard]] std::filesystem::path path_for_index(const std::size_t index) const
    {
        const std::string base_name = file_stem_ + "-" + current_date_ + ".log";
        if (index == 0U)
        {
            return directory_ / base_name;
        }
        return directory_ / (base_name + "." + std::to_string(index));
    }

    void open_for_date(std::string date)
    {
        file_.close();
        current_date_ = std::move(date);
        file_.open(path_for_index(0U).string(), false);
    }

    void rotate_current_file()
    {
        file_.close();
        std::error_code error;
        if (max_files_ == 1U)
        {
            std::filesystem::remove(path_for_index(0U), error);
            if (error)
            {
                throw spdlog::spdlog_ex{"cannot remove full log file", error.value()};
            }
        }
        else
        {
            std::filesystem::remove(path_for_index(max_files_ - 1U), error);
            error.clear();
            for (std::size_t index = max_files_ - 1U; index > 1U; --index)
            {
                const auto source = path_for_index(index - 1U);
                if (!std::filesystem::exists(source))
                {
                    continue;
                }
                std::filesystem::rename(source, path_for_index(index), error);
                if (error)
                {
                    throw spdlog::spdlog_ex{"cannot rotate log file", error.value()};
                }
            }
            const auto current = path_for_index(0U);
            if (std::filesystem::exists(current))
            {
                std::filesystem::rename(current, path_for_index(1U), error);
                if (error)
                {
                    throw spdlog::spdlog_ex{"cannot rotate current log file", error.value()};
                }
            }
        }
        file_.open(path_for_index(0U).string(), true);
    }

    std::filesystem::path directory_;
    std::string file_stem_;
    std::size_t max_file_size_;
    std::size_t max_files_;
    std::string current_date_;
    spdlog::details::file_helper file_;
};

spdlog::level::level_enum to_spdlog_level(const Level level) noexcept
{
    switch (level)
    {
    case Level::trace:
        return spdlog::level::trace;
    case Level::debug:
        return spdlog::level::debug;
    case Level::info:
        return spdlog::level::info;
    case Level::warning:
        return spdlog::level::warn;
    case Level::error:
        return spdlog::level::err;
    case Level::critical:
        return spdlog::level::critical;
    }
    return spdlog::level::err;
}

Error logging_error(std::string business_code, std::string message, std::string operation,
                    const std::error_code& native_error = {})
{
    Error error = make_error(std::move(business_code), Severity::error, std::move(message),
                             "logging", std::move(operation), true);
    if (native_error)
    {
        error.native_domain = "win32";
        error.native_code = std::to_string(native_error.value());
        error.details.push_back({"nativeMessage", native_error.message()});
    }
    return error;
}

} // namespace

LoggingRuntime::LoggingRuntime(ConstructorToken,
                               std::shared_ptr<spdlog::details::thread_pool> thread_pool,
                               std::shared_ptr<spdlog::logger> logger,
                               std::shared_ptr<RecentLogStore> recent_logs)
    : thread_pool_(std::move(thread_pool)), logger_(std::move(logger)),
      recent_logs_(std::move(recent_logs))
{
}

Result<std::unique_ptr<LoggingRuntime>> LoggingRuntime::create(const LoggingConfig& config)
{
    if (config.directory.empty() || config.file_stem.empty() || config.max_file_size_bytes == 0U ||
        config.max_files_per_day == 0U || config.queue_capacity == 0U ||
        config.recent_record_capacity == 0U)
    {
        return Result<std::unique_ptr<LoggingRuntime>>::failure(
            logging_error("LOG_INITIALIZATION_FAILED", "日志配置包含空路径、空文件名或零容量",
                          "logging.initialize"));
    }

    std::error_code directory_error;
    std::filesystem::create_directories(config.directory, directory_error);
    if (directory_error)
    {
        return Result<std::unique_ptr<LoggingRuntime>>::failure(
            logging_error("LOG_INITIALIZATION_FAILED", "无法创建日志目录",
                          "logging.createDirectory", directory_error));
    }

    try
    {
        auto recent_logs = std::make_shared<RecentLogStore>(config.recent_record_capacity);
        auto recent_sink = std::make_shared<RecentLogSink>(recent_logs);
        auto file_sink = std::make_shared<DailySizeRotatingFileSink>(
            config.directory, config.file_stem, config.max_file_size_bytes,
            config.max_files_per_day);
        const std::array<spdlog::sink_ptr, 2U> sinks{recent_sink, file_sink};
        auto thread_pool =
            std::make_shared<spdlog::details::thread_pool>(config.queue_capacity, 1U);
        auto logger = std::make_shared<spdlog::async_logger>(
            "paperbreak", sinks.begin(), sinks.end(), thread_pool,
            spdlog::async_overflow_policy::overrun_oldest);
        logger->set_level(to_spdlog_level(config.minimum_level));
        logger->set_pattern("%Y-%m-%dT%H:%M:%S.%e%z [%t] [%l] %v");
        logger->flush_on(spdlog::level::err);
        return Result<std::unique_ptr<LoggingRuntime>>::success(std::make_unique<LoggingRuntime>(
            ConstructorToken{}, std::move(thread_pool), std::move(logger), std::move(recent_logs)));
    }
    catch (const spdlog::spdlog_ex& exception)
    {
        Error error = logging_error("LOG_INITIALIZATION_FAILED", "无法初始化异步日志运行时",
                                    "logging.initialize");
        error.native_domain = "spdlog";
        error.details.push_back({"reason", redact_sensitive(exception.what())});
        return Result<std::unique_ptr<LoggingRuntime>>::failure(std::move(error));
    }
    catch (const std::exception& exception)
    {
        Error error = logging_error("LOG_INITIALIZATION_FAILED", "日志初始化发生未预期错误",
                                    "logging.initialize");
        error.details.push_back({"reason", redact_sensitive(exception.what())});
        return Result<std::unique_ptr<LoggingRuntime>>::failure(std::move(error));
    }
}

LoggingRuntime::~LoggingRuntime()
{
    static_cast<void>(shutdown());
}

Result<void> LoggingRuntime::log(const Category category, const Level level,
                                 const std::string_view message) noexcept
{
    std::scoped_lock lock{state_mutex_};
    if (stopped_ || !logger_)
    {
        return Result<void>::failure(
            logging_error("LOG_WRITE_FAILED", "日志运行时已经停止", "logging.write"));
    }

    try
    {
        logger_->log(to_spdlog_level(level), "[{}] {}", category_name(category),
                     redact_sensitive(message));
        return Result<void>::success();
    }
    catch (const std::exception& exception)
    {
        Error error =
            logging_error("LOG_WRITE_FAILED", "日志消息无法进入异步队列", "logging.write");
        error.details.push_back({"reason", redact_sensitive(exception.what())});
        return Result<void>::failure(std::move(error));
    }
}

Result<void> LoggingRuntime::shutdown() noexcept
{
    std::scoped_lock lock{state_mutex_};
    if (stopped_)
    {
        return Result<void>::success();
    }
    stopped_ = true;

    try
    {
        if (logger_)
        {
            logger_->flush();
        }
        logger_.reset();
        thread_pool_.reset();
        return Result<void>::success();
    }
    catch (const std::exception& exception)
    {
        logger_.reset();
        thread_pool_.reset();
        Error error =
            logging_error("LOG_WRITE_FAILED", "日志运行时关闭或刷新失败", "logging.shutdown");
        error.details.push_back({"reason", redact_sensitive(exception.what())});
        return Result<void>::failure(std::move(error));
    }
}

RecentLogQueryResult LoggingRuntime::tail(const RecentLogQuery& query) const
{
    if (!recent_logs_)
    {
        return {};
    }
    return recent_logs_->query(query);
}

std::string_view category_name(const Category category) noexcept
{
    switch (category)
    {
    case Category::service:
        return "service";
    case Category::camera:
        return "camera";
    case Category::algorithm:
        return "algorithm";
    case Category::event:
        return "event";
    case Category::storage:
        return "storage";
    case Category::uplink:
        return "uplink";
    case Category::ipc:
        return "ipc";
    case Category::ui:
        return "ui";
    case Category::audit:
        return "audit";
    case Category::performance:
        return "performance";
    }
    return "unknown";
}

std::optional<Category> parse_category(const std::string_view value) noexcept
{
    if (value == "service")
        return Category::service;
    if (value == "camera")
        return Category::camera;
    if (value == "algorithm")
        return Category::algorithm;
    if (value == "event")
        return Category::event;
    if (value == "storage")
        return Category::storage;
    if (value == "uplink")
        return Category::uplink;
    if (value == "ipc")
        return Category::ipc;
    if (value == "ui")
        return Category::ui;
    if (value == "audit")
        return Category::audit;
    if (value == "performance")
        return Category::performance;
    return std::nullopt;
}

std::string_view level_name(const Level level) noexcept
{
    switch (level)
    {
    case Level::trace:
        return "trace";
    case Level::debug:
        return "debug";
    case Level::info:
        return "info";
    case Level::warning:
        return "warning";
    case Level::error:
        return "error";
    case Level::critical:
        return "critical";
    }
    return "error";
}

std::optional<Level> parse_level(const std::string_view value) noexcept
{
    if (value == "trace")
        return Level::trace;
    if (value == "debug")
        return Level::debug;
    if (value == "info")
        return Level::info;
    if (value == "warning")
        return Level::warning;
    if (value == "error")
        return Level::error;
    if (value == "critical")
        return Level::critical;
    return std::nullopt;
}

std::string redact_sensitive(const std::string_view input)
{
    std::string value{input};
    static const std::regex json_secret{
        R"regex(("(?:password|token|secret|private[_-]?key)"\s*:\s*")([^"]*)("))regex",
        std::regex_constants::icase};
    static const std::regex key_value_secret{
        R"(((?:password|token|secret|private[_-]?key)\s*[:=]\s*)([^,;\s]+))",
        std::regex_constants::icase};
    value = std::regex_replace(value, json_secret, "$1***$3");
    value = std::regex_replace(value, key_value_secret, "$1***");
    return value;
}

} // namespace paperbreak::logging
