#include "paperbreak/common/error.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <utility>

namespace paperbreak
{

std::string current_utc_timestamp()
{
    const auto now = std::chrono::system_clock::now();
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
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

Error make_error(std::string business_code, const Severity severity, std::string message,
                 std::string module, std::string operation, const bool retryable)
{
    return Error{.business_code = std::move(business_code),
                 .severity = severity,
                 .message = std::move(message),
                 .module = std::move(module),
                 .operation = std::move(operation),
                 .source_id = std::nullopt,
                 .native_domain = std::nullopt,
                 .native_code = std::nullopt,
                 .details = {},
                 .retryable = retryable,
                 .timestamp = current_utc_timestamp(),
                 .correlation_id = std::nullopt};
}

} // namespace paperbreak
