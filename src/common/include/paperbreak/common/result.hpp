#pragma once

#include "paperbreak/common/error.hpp"

#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>

namespace paperbreak
{

/// Holds either one successful value or one stable business error.
template <typename T> class [[nodiscard]] Result final
{
  public:
    /// Creates a successful result.
    static Result success(T value)
    {
        return Result{std::move(value)};
    }
    /// Creates a failed result.
    static Result failure(Error error)
    {
        return Result{std::move(error)};
    }

    /// Reports whether the result contains a successful value.
    [[nodiscard]] bool has_value() const noexcept
    {
        return std::holds_alternative<T>(storage_);
    }
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return has_value();
    }

    /// Returns the successful value; throws std::bad_variant_access when failed.
    [[nodiscard]] T& value() &
    {
        return std::get<T>(storage_);
    }
    [[nodiscard]] const T& value() const&
    {
        return std::get<T>(storage_);
    }
    [[nodiscard]] T&& value() &&
    {
        return std::get<T>(std::move(storage_));
    }

    /// Returns the business error; throws std::bad_variant_access when successful.
    [[nodiscard]] Error& error() &
    {
        return std::get<Error>(storage_);
    }
    [[nodiscard]] const Error& error() const&
    {
        return std::get<Error>(storage_);
    }

  private:
    explicit Result(T value) : storage_(std::move(value)) {}
    explicit Result(Error error) : storage_(std::move(error)) {}

    std::variant<T, Error> storage_;
};

/// Result specialization for operations that have no success payload.
template <> class [[nodiscard]] Result<void> final
{
  public:
    /// Creates a successful result without a payload.
    static Result success()
    {
        return Result{};
    }
    /// Creates a failed result.
    static Result failure(Error error)
    {
        return Result{std::move(error)};
    }

    /// Reports whether the operation succeeded.
    [[nodiscard]] bool has_value() const noexcept
    {
        return !error_.has_value();
    }
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return has_value();
    }

    /// Returns the business error; throws std::bad_optional_access when successful.
    [[nodiscard]] Error& error() &
    {
        return error_.value();
    }
    [[nodiscard]] const Error& error() const&
    {
        return error_.value();
    }

  private:
    Result() = default;
    explicit Result(Error error) : error_(std::move(error)) {}

    std::optional<Error> error_;
};

} // namespace paperbreak
