#include "paperbreak/service/windows/console_control.hpp"

#include <Windows.h>
#include <gtest/gtest.h>

#include <optional>
#include <stdexcept>

TEST(WindowsControl, MapsInterruptAndShutdownControls)
{
    std::optional<paperbreak::service::StopReason> reason;
    const paperbreak::service::windows::StopCallback callback =
        [&reason](const paperbreak::service::StopReason value) { reason = value; };

    EXPECT_TRUE(paperbreak::service::windows::dispatch_console_control(CTRL_C_EVENT, callback));
    EXPECT_EQ(reason, paperbreak::service::StopReason::console_interrupt);
    EXPECT_TRUE(
        paperbreak::service::windows::dispatch_console_control(CTRL_SHUTDOWN_EVENT, callback));
    EXPECT_EQ(reason, paperbreak::service::StopReason::system_shutdown);
}

TEST(WindowsControl, RejectsUnknownControlsWithoutInvokingCallback)
{
    bool called = false;
    const paperbreak::service::windows::StopCallback callback =
        [&called](const paperbreak::service::StopReason) { called = true; };

    EXPECT_FALSE(paperbreak::service::windows::dispatch_console_control(999U, callback));
    EXPECT_FALSE(called);
}

TEST(WindowsControl, ContainsExceptionsAtTheWin32Boundary)
{
    const paperbreak::service::windows::StopCallback callback =
        [](const paperbreak::service::StopReason) { throw std::runtime_error{"injected"}; };

    EXPECT_FALSE(
        paperbreak::service::windows::dispatch_console_control(CTRL_CLOSE_EVENT, callback));
}
