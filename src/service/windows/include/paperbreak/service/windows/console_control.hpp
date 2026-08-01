#pragma once

#include "paperbreak/common/result.hpp"
#include "paperbreak/service/runtime.hpp"

#include <cstdint>
#include <functional>
#include <memory>

namespace paperbreak::service::windows
{

using StopCallback = std::function<void(StopReason)>;

struct ConsoleControlState;

/// Maps one Win32 console control value and invokes the callback behind an exception barrier.
[[nodiscard]] bool dispatch_console_control(std::uint32_t control_code,
                                            const StopCallback& callback) noexcept;

/// Process-scoped RAII registration for the Win32 console control callback.
class ConsoleControlRegistration final
{
    struct ConstructorToken final
    {
    };

  public:
    [[nodiscard]] static Result<std::unique_ptr<ConsoleControlRegistration>> create(
        StopCallback callback);

    ~ConsoleControlRegistration();

    ConsoleControlRegistration(const ConsoleControlRegistration&) = delete;
    ConsoleControlRegistration& operator=(const ConsoleControlRegistration&) = delete;
    ConsoleControlRegistration(ConsoleControlRegistration&&) = delete;
    ConsoleControlRegistration& operator=(ConsoleControlRegistration&&) = delete;

    [[nodiscard]] bool dispatch(std::uint32_t control_code) const noexcept;

    ConsoleControlRegistration(ConstructorToken, std::shared_ptr<ConsoleControlState> state);

  private:
    std::shared_ptr<ConsoleControlState> state_;
};

} // namespace paperbreak::service::windows
