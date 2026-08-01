#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace paperbreak
{

/// Stable severity used by errors, logs, alarms, and protocol responses.
enum class Severity
{
    info,
    warning,
    error,
    critical,
};

/// A bounded, pre-approved key/value item attached to an error.
struct ErrorDetail final
{
    std::string key;
    std::string value;
};

/// Stable business error with optional native diagnostics and structured context.
struct Error final
{
    std::string business_code;
    Severity severity{Severity::error};
    std::string message;
    std::string module;
    std::string operation;
    std::optional<std::string> source_id;
    std::optional<std::string> native_domain;
    std::optional<std::string> native_code;
    std::vector<ErrorDetail> details;
    bool retryable{false};
    std::string timestamp;
    std::optional<std::string> correlation_id;
};

/// Returns the current wall-clock time as UTC RFC 3339 with millisecond precision.
[[nodiscard]] std::string current_utc_timestamp();

/// Constructs required Error fields and stamps the instance with the current UTC time.
[[nodiscard]] Error make_error(std::string business_code, Severity severity, std::string message,
                               std::string module, std::string operation, bool retryable = false);

} // namespace paperbreak
