#include "paperbreak/logging/logging.hpp"

#include <spdlog/async.h>
#include <spdlog/details/file_helper.h>
#include <spdlog/details/thread_pool.h>
#include <spdlog/sinks/base_sink.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

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
            records_.pop_front();
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
            if (query.after_sequence && record.sequence <= *query.after_sequence)
                continue;
            if (!query.categories.empty() &&
                std::find(query.categories.begin(), query.categories.end(), record.category) ==
                    query.categories.end())
                continue;
            if (query.minimum_level &&
                static_cast<int>(record.level) < static_cast<int>(*query.minimum_level))
                continue;
            if (query.thread_name && record.thread_name != *query.thread_name)
                continue;
            matches.push_back(record);
        }

        if (query.after_sequence)
        {
            result.truncated =
                (!snapshot.empty() && *query.after_sequence < snapshot.front().sequence &&
                 snapshot.front().sequence - *query.after_sequence > 1U) ||
                matches.size() > limit;
            result.records.assign(matches.begin(),
                                  matches.begin() +
                                      static_cast<std::ptrdiff_t>(std::min(matches.size(), limit)));
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
constexpr std::size_t maximum_structured_fields = 16U;
constexpr std::size_t maximum_structured_value_bytes = 1024U;
constexpr std::string_view metadata_prefix{"\x1ePB1\x1f"};
constexpr char metadata_separator = '\x1f';

struct ParsedPayload final
{
    std::string thread_name;
    Category category{Category::service};
    std::string message;
};

[[nodiscard]] std::uint64_t current_thread_id() noexcept
{
#if defined(_WIN32)
    return static_cast<std::uint64_t>(::GetCurrentThreadId());
#else
    return static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}

void set_native_thread_description(const std::string_view name) noexcept
{
#if defined(_WIN32)
    if (name.empty())
    {
        static_cast<void>(::SetThreadDescription(::GetCurrentThread(), L""));
        return;
    }
    const int required = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(),
                                               static_cast<int>(name.size()), nullptr, 0);
    if (required <= 0)
        return;
    std::wstring wide(static_cast<std::size_t>(required), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(),
                              static_cast<int>(name.size()), wide.data(), required) == required)
        static_cast<void>(::SetThreadDescription(::GetCurrentThread(), wide.c_str()));
#else
    static_cast<void>(name);
#endif
}

[[nodiscard]] std::string local_date(const std::chrono::system_clock::time_point time)
{
    const std::time_t value = std::chrono::system_clock::to_time_t(time);
    std::tm local{};
#if defined(_WIN32)
    if (localtime_s(&local, &value) != 0)
#else
    if (localtime_r(&value, &local) == nullptr)
#endif
        throw spdlog::spdlog_ex{"cannot convert log timestamp to local date"};
    std::array<char, 11> buffer{};
    const int count = std::snprintf(buffer.data(), buffer.size(), "%04d-%02d-%02d",
                                    local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
    if (count != 10)
        throw spdlog::spdlog_ex{"cannot format log date"};
    return std::string{buffer.data(), 10U};
}

[[nodiscard]] Level from_spdlog_level(const spdlog::level::level_enum level) noexcept
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

[[nodiscard]] spdlog::level::level_enum to_spdlog_level(const Level level) noexcept
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

[[nodiscard]] ParsedPayload parse_payload(const spdlog::details::log_msg& log_message)
{
    const std::string_view payload{log_message.payload.data(), log_message.payload.size()};
    if (!payload.starts_with(metadata_prefix))
    {
        return {.thread_name = "unregistered-thread-" + std::to_string(log_message.thread_id),
                .message = std::string{payload}};
    }
    const std::size_t thread_begin = metadata_prefix.size();
    const std::size_t thread_end = payload.find(metadata_separator, thread_begin);
    const std::size_t category_end = thread_end == std::string_view::npos
                                         ? std::string_view::npos
                                         : payload.find(metadata_separator, thread_end + 1U);
    if (thread_end == std::string_view::npos || category_end == std::string_view::npos)
    {
        return {.thread_name = "unregistered-thread-" + std::to_string(log_message.thread_id),
                .message = "businessCode=LOG_WRITE_FAILED malformed internal log metadata"};
    }
    const auto parsed_category =
        parse_category(payload.substr(thread_end + 1U, category_end - thread_end - 1U));
    return {.thread_name = std::string{payload.substr(thread_begin, thread_end - thread_begin)},
            .category = parsed_category.value_or(Category::service),
            .message = std::string{payload.substr(category_end + 1U)}};
}

[[nodiscard]] std::string formatted_line(const spdlog::details::log_msg& log_message,
                                         const ParsedPayload& payload)
{
    std::string line = local_rfc3339_timestamp(log_message.time);
    line += " [" + std::to_string(log_message.thread_id) + "] [" + payload.thread_name + "] [";
    line += level_name(from_spdlog_level(log_message.level));
    line += "] [";
    line += category_name(payload.category);
    line += "] ";
    line += payload.message;
    line.push_back('\n');
    return line;
}

void write_text(spdlog::details::file_helper& file, const std::string_view text)
{
    spdlog::memory_buf_t buffer;
    buffer.append(text.data(), text.data() + text.size());
    file.write(buffer);
}

class RecentLogSink final : public spdlog::sinks::base_sink<std::mutex>
{
  public:
    explicit RecentLogSink(std::shared_ptr<RecentLogStore> store) : store_(std::move(store)) {}

  protected:
    void sink_it_(const spdlog::details::log_msg& log_message) override
    {
        auto payload = parse_payload(log_message);
        if (payload.message.size() > maximum_recent_message_bytes)
            payload.message.resize(maximum_recent_message_bytes);
        store_->append({.timestamp = local_rfc3339_timestamp(log_message.time),
                        .thread_id = log_message.thread_id,
                        .thread_name = std::move(payload.thread_name),
                        .category = payload.category,
                        .level = from_spdlog_level(log_message.level),
                        .message = std::move(payload.message)});
    }

    void flush_() override {}

  private:
    std::shared_ptr<RecentLogStore> store_;
};

class ThreadRoutingFileSink final : public spdlog::sinks::base_sink<std::mutex>
{
  public:
    ThreadRoutingFileSink(std::filesystem::path directory, std::string file_stem,
                          const std::size_t max_file_size, const std::size_t max_files,
                          const std::size_t maximum_states, const std::uint32_t retention_days)
        : directory_(std::move(directory)), file_stem_(std::move(file_stem)),
          max_file_size_(max_file_size), max_files_(max_files), maximum_states_(maximum_states),
          retention_days_(retention_days)
    {
    }

    void set_retention_days(const std::uint32_t days) noexcept
    {
        retention_days_.store(days, std::memory_order_release);
        retention_generation_.fetch_add(1U, std::memory_order_acq_rel);
    }

  protected:
    void sink_it_(const spdlog::details::log_msg& message) override
    {
        cleanup_if_needed(message.time);
        auto payload = parse_payload(message);
        if (!valid_thread_name(payload.thread_name) &&
            !payload.thread_name.starts_with("unregistered-thread-"))
            payload.thread_name = "unregistered-thread-" + std::to_string(message.thread_id);

        auto iterator = files_.find(payload.thread_name);
        if (iterator == files_.end() && files_.size() >= maximum_states_)
        {
            payload.thread_name = "unregistered-thread-" + std::to_string(message.thread_id);
            payload.category = Category::service;
            payload.message =
                "businessCode=LOG_THREAD_FILE_LIMIT_REACHED result=degraded maximumStates=" +
                std::to_string(maximum_states_) + " originalMessage=" + payload.message;
            write_emergency(message, payload);
            return;
        }
        if (iterator == files_.end())
            iterator = files_.try_emplace(payload.thread_name).first;

        auto& state = iterator->second;
        const std::string message_date = local_date(message.time);
        if (!state.open || state.date != message_date)
            open_for_date(state, payload.thread_name, message_date);
        const std::string line = formatted_line(message, payload);
        if (state.file.size() + line.size() > max_file_size_)
            rotate_current_file(state, payload.thread_name);
        write_text(state.file, line);
    }

    void flush_() override
    {
        for (auto& [name, state] : files_)
        {
            static_cast<void>(name);
            if (state.open)
                state.file.flush();
        }
    }

  private:
    struct FileState final
    {
        std::string date;
        spdlog::details::file_helper file;
        bool open{};
    };

    [[nodiscard]] std::filesystem::path path_for_index(const std::string_view thread_name,
                                                       const std::string_view date,
                                                       const std::size_t index) const
    {
        std::string base_name =
            file_stem_ + "-" + std::string{thread_name} + "-" + std::string{date} + ".log";
        if (index != 0U)
            base_name += "." + std::to_string(index);
        return directory_ / base_name;
    }

    void open_for_date(FileState& state, const std::string_view thread_name, std::string date)
    {
        state.file.close();
        state.date = std::move(date);
        state.file.open(path_for_index(thread_name, state.date, 0U).string(), false);
        state.open = true;
    }

    void rotate_current_file(FileState& state, const std::string_view thread_name)
    {
        state.file.close();
        state.open = false;
        std::error_code error;
        if (max_files_ == 1U)
        {
            std::filesystem::remove(path_for_index(thread_name, state.date, 0U), error);
            if (error)
                throw spdlog::spdlog_ex{"cannot remove full thread log file", error.value()};
        }
        else
        {
            std::filesystem::remove(path_for_index(thread_name, state.date, max_files_ - 1U),
                                    error);
            error.clear();
            for (std::size_t index = max_files_ - 1U; index > 1U; --index)
            {
                const auto source = path_for_index(thread_name, state.date, index - 1U);
                if (!std::filesystem::exists(source))
                    continue;
                std::filesystem::rename(source, path_for_index(thread_name, state.date, index),
                                        error);
                if (error)
                    throw spdlog::spdlog_ex{"cannot rotate thread log file", error.value()};
            }
            const auto current = path_for_index(thread_name, state.date, 0U);
            if (std::filesystem::exists(current))
            {
                std::filesystem::rename(current, path_for_index(thread_name, state.date, 1U),
                                        error);
                if (error)
                    throw spdlog::spdlog_ex{"cannot rotate current thread log file", error.value()};
            }
        }
        state.file.open(path_for_index(thread_name, state.date, 0U).string(), true);
        state.open = true;
    }

    void write_emergency(const spdlog::details::log_msg& message, const ParsedPayload& payload)
    {
        spdlog::details::file_helper emergency;
        emergency.open(path_for_index(payload.thread_name, local_date(message.time), 0U).string(),
                       false);
        write_text(emergency, formatted_line(message, payload));
        emergency.flush();
    }

    void cleanup_if_needed(const std::chrono::system_clock::time_point now)
    {
        const std::string today = local_date(now);
        const std::uint64_t generation = retention_generation_.load(std::memory_order_acquire);
        if (today == cleanup_date_ && generation == observed_retention_generation_)
            return;
        cleanup_date_ = today;
        observed_retention_generation_ = generation;

        if (file_stem_ != "paperbreak-service" && file_stem_ != "paperbreak-console")
            return;
        const auto cutoff =
            now - std::chrono::days{retention_days_.load(std::memory_order_acquire)};
        const std::regex recognized{"^" + file_stem_ +
                                    R"(-[a-z0-9-]{1,63}-(\d{4}-\d{2}-\d{2})\.log(?:\.\d+)?$)"};
        std::error_code iterator_error;
        for (std::filesystem::directory_iterator iterator{directory_, iterator_error}, end;
             !iterator_error && iterator != end; iterator.increment(iterator_error))
        {
            if (!iterator->is_regular_file())
                continue;
            std::smatch match;
            const std::string filename = iterator->path().filename().string();
            if (!std::regex_match(filename, match, recognized))
                continue;
            std::tm date{};
            if (sscanf_s(match[1].str().c_str(), "%d-%d-%d", &date.tm_year, &date.tm_mon,
                         &date.tm_mday) != 3)
                continue;
            date.tm_year -= 1900;
            date.tm_mon -= 1;
            date.tm_hour = 12;
            const std::time_t local_value = std::mktime(&date);
            if (local_value == static_cast<std::time_t>(-1) ||
                std::chrono::system_clock::from_time_t(local_value) >= cutoff)
                continue;
            std::error_code remove_error;
            std::filesystem::remove(iterator->path(), remove_error);
        }
    }

    std::filesystem::path directory_;
    std::string file_stem_;
    std::size_t max_file_size_;
    std::size_t max_files_;
    std::size_t maximum_states_;
    std::atomic<std::uint32_t> retention_days_;
    std::atomic<std::uint64_t> retention_generation_{0U};
    std::uint64_t observed_retention_generation_{};
    std::string cleanup_date_;
    std::map<std::string, FileState, std::less<>> files_;
};

[[nodiscard]] Error logging_error(std::string business_code, std::string message,
                                  std::string operation, const std::error_code& native_error = {})
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

[[nodiscard]] std::string bounded_value(const std::string_view value)
{
    const std::size_t size = std::min(value.size(), maximum_structured_value_bytes);
    return redact_sensitive(value.substr(0U, size));
}

} // namespace

struct LoggingRuntime::State final
{
    mutable std::mutex mutex;
    mutable std::mutex registry_mutex;
    std::shared_ptr<spdlog::details::thread_pool> thread_pool;
    std::shared_ptr<spdlog::logger> logger;
    std::shared_ptr<RecentLogStore> recent_logs;
    std::shared_ptr<ThreadRoutingFileSink> file_sink;
    std::unordered_map<std::uint64_t, std::string> thread_names;
    std::set<std::string, std::less<>> active_names;
    std::atomic<Level> minimum_level{Level::info};
    std::atomic<std::uint32_t> retention_days{30U};
    std::atomic<std::uint64_t> final_overrun_count{0U};
    std::atomic_bool stopped{false};

    [[nodiscard]] std::string name_for_current_thread() const
    {
        const auto id = current_thread_id();
        std::scoped_lock lock{registry_mutex};
        const auto iterator = thread_names.find(id);
        return iterator == thread_names.end() ? "unregistered-thread-" + std::to_string(id)
                                              : iterator->second;
    }

    [[nodiscard]] Result<void> enqueue(
        const Category category, const Level level, const std::string_view message,
        const std::optional<std::string_view> forced_thread_name = std::nullopt) noexcept
    {
        std::scoped_lock lock{mutex};
        if (stopped.load(std::memory_order_acquire) || !logger)
            return Result<void>::failure(
                logging_error("LOG_WRITE_FAILED", "日志运行时已经停止", "logging.write"));
        try
        {
            const std::string payload =
                std::string{metadata_prefix} +
                std::string{forced_thread_name.value_or(name_for_current_thread())} +
                metadata_separator + std::string{category_name(category)} + metadata_separator +
                redact_sensitive(message);
            logger->log(to_spdlog_level(level), payload);
            return Result<void>::success();
        }
        catch (const std::exception& exception)
        {
            auto error =
                logging_error("LOG_WRITE_FAILED", "日志消息无法进入异步队列", "logging.write");
            error.details.push_back({"reason", redact_sensitive(exception.what())});
            return Result<void>::failure(std::move(error));
        }
    }

    void unregister_thread(const std::uint64_t id, const std::string_view name) noexcept
    {
        static_cast<void>(
            enqueue(Category::service, Level::info,
                    "operation=thread.stop result=success threadName=" + std::string{name}));
        std::scoped_lock lock{registry_mutex};
        const auto iterator = thread_names.find(id);
        if (iterator != thread_names.end() && iterator->second == name)
        {
            active_names.erase(iterator->second);
            thread_names.erase(iterator);
        }
        set_native_thread_description("");
    }
};

LoggingRuntime::ThreadRegistration::ThreadRegistration(std::weak_ptr<State> state,
                                                       const std::uint64_t thread_id,
                                                       std::string name)
    : state_(std::move(state)), thread_id_(thread_id), name_(std::move(name))
{
}

LoggingRuntime::ThreadRegistration::~ThreadRegistration()
{
    reset();
}

LoggingRuntime::ThreadRegistration::ThreadRegistration(ThreadRegistration&& other) noexcept
    : state_(std::move(other.state_)), thread_id_(std::exchange(other.thread_id_, 0U)),
      name_(std::move(other.name_))
{
}

LoggingRuntime::ThreadRegistration& LoggingRuntime::ThreadRegistration::operator=(
    ThreadRegistration&& other) noexcept
{
    if (this != &other)
    {
        reset();
        state_ = std::move(other.state_);
        thread_id_ = std::exchange(other.thread_id_, 0U);
        name_ = std::move(other.name_);
    }
    return *this;
}

std::string_view LoggingRuntime::ThreadRegistration::name() const noexcept
{
    return name_;
}

LoggingRuntime::ThreadRegistration::operator bool() const noexcept
{
    return thread_id_ != 0U;
}

void LoggingRuntime::ThreadRegistration::reset() noexcept
{
    if (thread_id_ == 0U)
        return;
    if (auto state = state_.lock())
        state->unregister_thread(thread_id_, name_);
    thread_id_ = 0U;
    name_.clear();
}

LoggingRuntime::LoggingRuntime(std::shared_ptr<State> state) : state_(std::move(state)) {}

Result<std::unique_ptr<LoggingRuntime>> LoggingRuntime::create(const LoggingConfig& config)
{
    if (config.directory.empty() || config.file_stem.empty() || config.max_file_size_bytes == 0U ||
        config.max_files_per_day == 0U || config.queue_capacity == 0U ||
        config.recent_record_capacity == 0U || config.maximum_thread_file_states == 0U ||
        config.maximum_thread_file_states > 64U || config.retention_days == 0U ||
        config.retention_days > 3650U)
        return Result<std::unique_ptr<LoggingRuntime>>::failure(
            logging_error("LOG_INITIALIZATION_FAILED", "日志配置包含空路径、空文件名或无效容量",
                          "logging.initialize"));

    std::error_code directory_error;
    std::filesystem::create_directories(config.directory, directory_error);
    if (directory_error)
        return Result<std::unique_ptr<LoggingRuntime>>::failure(
            logging_error("LOG_INITIALIZATION_FAILED", "无法创建日志目录",
                          "logging.createDirectory", directory_error));

    try
    {
        auto state = std::make_shared<State>();
        state->recent_logs = std::make_shared<RecentLogStore>(config.recent_record_capacity);
        auto recent_sink = std::make_shared<RecentLogSink>(state->recent_logs);
        state->file_sink = std::make_shared<ThreadRoutingFileSink>(
            config.directory, config.file_stem, config.max_file_size_bytes,
            config.max_files_per_day, config.maximum_thread_file_states, config.retention_days);
        const std::array<spdlog::sink_ptr, 2U> sinks{recent_sink, state->file_sink};
        state->thread_pool = std::make_shared<spdlog::details::thread_pool>(
            config.queue_capacity, 1U, [] { set_native_thread_description("logging-worker"); },
            [] { set_native_thread_description(""); });
        state->logger = std::make_shared<spdlog::async_logger>(
            "paperbreak", sinks.begin(), sinks.end(), state->thread_pool,
            spdlog::async_overflow_policy::overrun_oldest);
        state->minimum_level.store(config.minimum_level, std::memory_order_release);
        state->retention_days.store(config.retention_days, std::memory_order_release);
        state->logger->set_level(to_spdlog_level(config.minimum_level));
        state->logger->flush_on(spdlog::level::err);
        return Result<std::unique_ptr<LoggingRuntime>>::success(
            std::unique_ptr<LoggingRuntime>{new LoggingRuntime{std::move(state)}});
    }
    catch (const std::exception& exception)
    {
        auto error = logging_error("LOG_INITIALIZATION_FAILED", "无法初始化异步日志运行时",
                                   "logging.initialize");
        error.native_domain = "spdlog";
        error.details.push_back({"reason", redact_sensitive(exception.what())});
        return Result<std::unique_ptr<LoggingRuntime>>::failure(std::move(error));
    }
}

LoggingRuntime::~LoggingRuntime()
{
    static_cast<void>(shutdown());
}

Result<LoggingRuntime::ThreadRegistration> LoggingRuntime::register_current_thread(
    const std::string_view name)
{
    const auto id = current_thread_id();
    if (!valid_thread_name(name))
    {
        static_cast<void>(state_->enqueue(
            Category::service, Level::error,
            "operation=thread.register result=failure businessCode=LOG_THREAD_REGISTRATION_FAILED "
            "reason=invalid-name requestedName=" +
                bounded_value(name),
            "unregistered-thread-" + std::to_string(id)));
        return Result<ThreadRegistration>::failure(logging_error(
            "LOG_THREAD_REGISTRATION_FAILED", "线程名必须为 1～63 个小写字母、数字或连字符",
            "logging.thread.register"));
    }

    bool duplicate = false;
    {
        std::scoped_lock lock{state_->registry_mutex};
        if (state_->thread_names.contains(id) || state_->active_names.contains(name))
            duplicate = true;
        else
        {
            state_->thread_names.emplace(id, name);
            state_->active_names.emplace(name);
        }
    }
    if (duplicate)
    {
        static_cast<void>(state_->enqueue(
            Category::service, Level::error,
            "operation=thread.register result=failure "
            "businessCode=LOG_THREAD_REGISTRATION_FAILED reason=duplicate-name requestedName=" +
                std::string{name},
            "unregistered-thread-" + std::to_string(id)));
        return Result<ThreadRegistration>::failure(
            logging_error("LOG_THREAD_REGISTRATION_FAILED", "线程名已被当前进程中的活动线程注册",
                          "logging.thread.register"));
    }
    set_native_thread_description(name);
    static_cast<void>(
        state_->enqueue(Category::service, Level::info,
                        "operation=thread.start result=success threadName=" + std::string{name}));
    return Result<ThreadRegistration>::success(ThreadRegistration{state_, id, std::string{name}});
}

bool LoggingRuntime::enabled(const Level level) const noexcept
{
    return state_ && !state_->stopped.load(std::memory_order_acquire) &&
           static_cast<int>(level) >=
               static_cast<int>(state_->minimum_level.load(std::memory_order_acquire));
}

Result<void> LoggingRuntime::set_minimum_level(const Level level) noexcept
{
    std::scoped_lock lock{state_->mutex};
    if (state_->stopped.load(std::memory_order_acquire) || !state_->logger)
        return Result<void>::failure(
            logging_error("LOG_WRITE_FAILED", "日志运行时已经停止", "logging.setLevel"));
    state_->logger->set_level(to_spdlog_level(level));
    state_->minimum_level.store(level, std::memory_order_release);
    return Result<void>::success();
}

Level LoggingRuntime::minimum_level() const noexcept
{
    return state_->minimum_level.load(std::memory_order_acquire);
}

Result<void> LoggingRuntime::set_retention_days(const std::uint32_t days) noexcept
{
    if (days == 0U || days > 3650U)
        return Result<void>::failure(
            logging_error("LOG_WRITE_FAILED", "日志保留天数超出范围", "logging.setRetention"));
    state_->retention_days.store(days, std::memory_order_release);
    state_->file_sink->set_retention_days(days);
    return Result<void>::success();
}

std::uint32_t LoggingRuntime::retention_days() const noexcept
{
    return state_->retention_days.load(std::memory_order_acquire);
}

Result<void> LoggingRuntime::log(const Category category, const Level level,
                                 const std::string_view message) noexcept
{
    if (!enabled(level))
        return Result<void>::success();
    return state_->enqueue(category, level, message);
}

Result<void> LoggingRuntime::log(const StructuredLog& record) noexcept
{
    if (record.fields.size() > maximum_structured_fields)
        return Result<void>::failure(logging_error("LOG_WRITE_FAILED", "结构化日志字段超过 16 项",
                                                   "logging.writeStructured"));
    if (!enabled(record.level))
        return Result<void>::success();
    std::string message;
    message.reserve(256U);
    const auto append = [&message](const std::string_view key, const std::string_view value) {
        if (value.empty())
            return;
        if (!message.empty())
            message.push_back(' ');
        message += key;
        message.push_back('=');
        message += bounded_value(value);
    };
    append("operation", record.operation);
    append("result", record.result);
    append("businessCode", record.business_code);
    append("correlationId", record.correlation_id);
    for (const auto& field : record.fields)
        append(bounded_value(field.key), field.value);
    return state_->enqueue(record.category, record.level, message);
}

Result<void> LoggingRuntime::shutdown() noexcept
{
    if (!state_)
        return Result<void>::success();
    std::scoped_lock lock{state_->mutex};
    if (state_->stopped.load(std::memory_order_acquire))
        return Result<void>::success();
    state_->stopped.store(true, std::memory_order_release);
    try
    {
        if (state_->logger)
            state_->logger->flush();
        if (state_->thread_pool)
            state_->final_overrun_count.store(state_->thread_pool->overrun_counter(),
                                              std::memory_order_release);
        state_->logger.reset();
        state_->thread_pool.reset();
        return Result<void>::success();
    }
    catch (const std::exception& exception)
    {
        state_->logger.reset();
        state_->thread_pool.reset();
        auto error =
            logging_error("LOG_WRITE_FAILED", "日志运行时关闭或刷新失败", "logging.shutdown");
        error.details.push_back({"reason", redact_sensitive(exception.what())});
        return Result<void>::failure(std::move(error));
    }
}

RecentLogQueryResult LoggingRuntime::tail(const RecentLogQuery& query) const
{
    return state_ && state_->recent_logs ? state_->recent_logs->query(query)
                                         : RecentLogQueryResult{};
}

std::uint64_t LoggingRuntime::overrun_count() const noexcept
{
    std::scoped_lock lock{state_->mutex};
    return state_->thread_pool ? state_->thread_pool->overrun_counter()
                               : state_->final_overrun_count.load(std::memory_order_acquire);
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

bool valid_thread_name(const std::string_view value) noexcept
{
    return !value.empty() && value.size() <= 63U &&
           std::all_of(value.begin(), value.end(), [](const unsigned char character) {
               return std::islower(character) != 0 || std::isdigit(character) != 0 ||
                      character == '-';
           });
}

std::string local_rfc3339_timestamp(const std::chrono::system_clock::time_point time)
{
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch());
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(milliseconds);
    const auto fraction = milliseconds - seconds;
    const std::time_t value =
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::time_point{seconds});
    std::tm local{};
#if defined(_WIN32)
    if (localtime_s(&local, &value) != 0)
#else
    if (localtime_r(&value, &local) == nullptr)
#endif
        return "1970-01-01T00:00:00.000+00:00";

#if defined(_WIN32)
    const std::time_t local_as_utc = _mkgmtime(&local);
#else
    const std::time_t local_as_utc = timegm(&local);
#endif
    const long long offset_seconds = static_cast<long long>(local_as_utc - value);
    const char sign = offset_seconds < 0 ? '-' : '+';
    const long long absolute_offset = offset_seconds < 0 ? -offset_seconds : offset_seconds;
    const long long offset_hours = absolute_offset / 3600;
    const long long offset_minutes = (absolute_offset % 3600) / 60;
    std::array<char, 40> buffer{};
    const int count = std::snprintf(
        buffer.data(), buffer.size(), "%04d-%02d-%02dT%02d:%02d:%02d.%03lld%c%02lld:%02lld",
        local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min,
        local.tm_sec, static_cast<long long>(fraction.count()), sign, offset_hours, offset_minutes);
    if (count <= 0 || static_cast<std::size_t>(count) >= buffer.size())
        return "1970-01-01T00:00:00.000+00:00";
    return std::string{buffer.data(), static_cast<std::size_t>(count)};
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
