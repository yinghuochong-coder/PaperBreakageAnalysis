#include "paperbreak/service/windows/console_control.hpp"

#include <Windows.h>

#include <atomic>
#include <optional>
#include <string>
#include <utility>

namespace paperbreak::service::windows
{

struct ConsoleControlState final
{
    StopCallback callback;
};

namespace
{

std::atomic<std::shared_ptr<ConsoleControlState>> active_state;

BOOL WINAPI console_control_handler(const DWORD control_code) noexcept
{
    const auto state = active_state.load(std::memory_order_acquire);
    if (!state)
    {
        return FALSE;
    }
    return dispatch_console_control(control_code, state->callback) ? TRUE : FALSE;
}

std::optional<StopReason> map_control_code(const std::uint32_t control_code) noexcept
{
    switch (control_code)
    {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
        return StopReason::console_interrupt;
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        return StopReason::system_shutdown;
    default:
        return std::nullopt;
    }
}

} // namespace

bool dispatch_console_control(const std::uint32_t control_code,
                              const StopCallback& callback) noexcept
{
    const auto reason = map_control_code(control_code);
    if (!reason.has_value() || !callback)
    {
        return false;
    }

    try
    {
        callback(reason.value());
        return true;
    }
    catch (...)
    {
        return false;
    }
}

Result<std::unique_ptr<ConsoleControlRegistration>> ConsoleControlRegistration::create(
    StopCallback callback)
{
    if (!callback)
    {
        return Result<std::unique_ptr<ConsoleControlRegistration>>::failure(
            make_error("SYS_SERVICE_START_FAILED", Severity::critical, "控制台停止回调为空",
                       "service", "service.consoleControl.register"));
    }

    auto state = std::make_shared<ConsoleControlState>(ConsoleControlState{std::move(callback)});
    std::shared_ptr<ConsoleControlState> expected;
    if (!active_state.compare_exchange_strong(expected, state, std::memory_order_acq_rel))
    {
        return Result<std::unique_ptr<ConsoleControlRegistration>>::failure(
            make_error("SYS_SERVICE_START_FAILED", Severity::critical, "控制台控制回调已经注册",
                       "service", "service.consoleControl.register"));
    }

    if (SetConsoleCtrlHandler(console_control_handler, TRUE) == FALSE)
    {
        active_state.store({}, std::memory_order_release);
        const DWORD native_code = GetLastError();
        Error error =
            make_error("SYS_SERVICE_START_FAILED", Severity::critical, "无法注册控制台控制回调",
                       "service", "service.consoleControl.register");
        error.native_domain = "win32";
        error.native_code = std::to_string(native_code);
        return Result<std::unique_ptr<ConsoleControlRegistration>>::failure(std::move(error));
    }

    return Result<std::unique_ptr<ConsoleControlRegistration>>::success(
        std::make_unique<ConsoleControlRegistration>(ConstructorToken{}, std::move(state)));
}

ConsoleControlRegistration::ConsoleControlRegistration(ConstructorToken,
                                                       std::shared_ptr<ConsoleControlState> state)
    : state_(std::move(state))
{
}

ConsoleControlRegistration::~ConsoleControlRegistration()
{
    auto expected = state_;
    static_cast<void>(
        active_state.compare_exchange_strong(expected, {}, std::memory_order_acq_rel));
    static_cast<void>(SetConsoleCtrlHandler(console_control_handler, FALSE));
    state_.reset();
}

bool ConsoleControlRegistration::dispatch(const std::uint32_t control_code) const noexcept
{
    return state_ && dispatch_console_control(control_code, state_->callback);
}

} // namespace paperbreak::service::windows
