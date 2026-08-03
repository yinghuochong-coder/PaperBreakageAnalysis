#include "paperbreak/service/windows/scm.hpp"
#include "paperbreak/service/windows/scm_host.hpp"

#include <Windows.h>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace
{

paperbreak::Error fake_error(const std::string_view business_code, const std::string_view operation)
{
    return paperbreak::make_error(std::string{business_code}, paperbreak::Severity::error,
                                  "injected", "service", std::string{operation});
}

class FakeServiceManagerApi final : public paperbreak::service::windows::IServiceManagerApi
{
  public:
    [[nodiscard]] paperbreak::Result<void> verify_management_access() override
    {
        ++verify_access_calls;
        if (fail_access)
        {
            auto error = fake_error("SYS_SERVICE_CONTROL_FAILED", "fake.verifyAccess");
            error.native_domain = "win32";
            error.native_code = std::to_string(ERROR_ACCESS_DENIED);
            return paperbreak::Result<void>::failure(std::move(error));
        }
        return paperbreak::Result<void>::success();
    }

    [[nodiscard]] paperbreak::Result<paperbreak::service::windows::ServicePresence> query(
        std::string_view) override
    {
        if (fail_query)
        {
            auto error = fake_error("SYS_SERVICE_INSTALL_FAILED", "fake.query");
            error.native_domain = "win32";
            error.native_code = std::to_string(ERROR_ACCESS_DENIED);
            return paperbreak::Result<paperbreak::service::windows::ServicePresence>::failure(
                std::move(error));
        }
        return paperbreak::Result<paperbreak::service::windows::ServicePresence>::success(presence);
    }

    [[nodiscard]] paperbreak::Result<void> create(
        const paperbreak::service::windows::ServiceDefinition& definition) override
    {
        ++create_calls;
        last_definition = definition;
        if (fail_create)
        {
            return paperbreak::Result<void>::failure(
                fake_error("SYS_SERVICE_INSTALL_FAILED", "fake.create"));
        }
        presence = paperbreak::service::windows::ServicePresence::present;
        return paperbreak::Result<void>::success();
    }

    [[nodiscard]] paperbreak::Result<void> update(
        const paperbreak::service::windows::ServiceDefinition& definition) override
    {
        ++update_calls;
        last_definition = definition;
        if (fail_update)
        {
            return paperbreak::Result<void>::failure(
                fake_error("SYS_SERVICE_INSTALL_FAILED", "fake.update"));
        }
        return paperbreak::Result<void>::success();
    }

    [[nodiscard]] paperbreak::Result<void> configure(
        const paperbreak::service::windows::ServiceDefinition& definition) override
    {
        ++configure_calls;
        last_definition = definition;
        if (fail_configure)
        {
            return paperbreak::Result<void>::failure(
                fake_error("SYS_SERVICE_INSTALL_FAILED", "fake.configure"));
        }
        return paperbreak::Result<void>::success();
    }

    [[nodiscard]] paperbreak::Result<paperbreak::service::windows::ManagedServiceState> query_state(
        std::string_view) override
    {
        ++query_state_calls;
        return paperbreak::Result<paperbreak::service::windows::ManagedServiceState>::success(
            state);
    }

    [[nodiscard]] paperbreak::Result<void> request_stop(std::string_view) override
    {
        ++stop_calls;
        if (stop_transitions)
            state = paperbreak::service::windows::ManagedServiceState::stopped;
        return paperbreak::Result<void>::success();
    }

    [[nodiscard]] paperbreak::Result<void> request_start(std::string_view) override
    {
        ++start_calls;
        if (fail_start)
            return paperbreak::Result<void>::failure(
                fake_error("SYS_SERVICE_RESTART_FAILED", "fake.start"));
        state = paperbreak::service::windows::ManagedServiceState::running;
        return paperbreak::Result<void>::success();
    }

    [[nodiscard]] paperbreak::Result<bool> wait_for_stopped(
        std::string_view, const std::chrono::milliseconds timeout) override
    {
        ++wait_calls;
        last_wait = timeout;
        return paperbreak::Result<bool>::success(wait_succeeds);
    }

    [[nodiscard]] paperbreak::Result<void> remove(std::string_view) override
    {
        ++remove_calls;
        if (fail_remove)
        {
            return paperbreak::Result<void>::failure(
                fake_error("SYS_SERVICE_UNINSTALL_FAILED", "fake.remove"));
        }
        presence = paperbreak::service::windows::ServicePresence::missing;
        return paperbreak::Result<void>::success();
    }

    paperbreak::service::windows::ServicePresence presence{
        paperbreak::service::windows::ServicePresence::missing};
    paperbreak::service::windows::ManagedServiceState state{
        paperbreak::service::windows::ManagedServiceState::stopped};
    paperbreak::service::windows::ServiceDefinition last_definition;
    std::chrono::milliseconds last_wait{0};
    int create_calls{0};
    int verify_access_calls{0};
    int update_calls{0};
    int configure_calls{0};
    int query_state_calls{0};
    int stop_calls{0};
    int start_calls{0};
    int wait_calls{0};
    int remove_calls{0};
    bool wait_succeeds{true};
    bool fail_access{false};
    bool fail_query{false};
    bool fail_create{false};
    bool fail_update{false};
    bool fail_configure{false};
    bool fail_remove{false};
    bool fail_start{false};
    bool stop_transitions{true};
};

class FakeHostedService final : public paperbreak::service::windows::IHostedService
{
  public:
    [[nodiscard]] paperbreak::Result<paperbreak::service::StartOutcome> start() override
    {
        return paperbreak::Result<paperbreak::service::StartOutcome>::success(
            paperbreak::service::StartOutcome::running);
    }

    void request_stop(const paperbreak::service::StopReason reason) override
    {
        if (throw_on_stop)
        {
            throw std::runtime_error{"injected"};
        }
        stop_reason = reason;
    }

    [[nodiscard]] paperbreak::Result<void> shutdown() override
    {
        return paperbreak::Result<void>::success();
    }

    std::optional<paperbreak::service::StopReason> stop_reason;
    bool throw_on_stop{false};
};

} // namespace

TEST(WindowsScmManager, CreatesAndConfiguresTheServiceDefinition)
{
    FakeServiceManagerApi api;
    paperbreak::service::windows::ServiceManager manager{api};
    paperbreak::service::windows::ServiceDefinition definition;
    definition.command_line = L"service command";

    auto result = manager.install(definition);

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value(), paperbreak::service::windows::InstallOutcome::created);
    EXPECT_EQ(api.create_calls, 1);
    EXPECT_EQ(api.update_calls, 0);
    EXPECT_EQ(api.configure_calls, 1);
    EXPECT_EQ(api.last_definition.account, "NT AUTHORITY\\LocalService");
    ASSERT_EQ(api.last_definition.restart_delays.size(), 3U);
    EXPECT_EQ(api.last_definition.restart_delays[0], std::chrono::seconds{5});
    EXPECT_EQ(api.last_definition.restart_delays[1], std::chrono::seconds{15});
    EXPECT_EQ(api.last_definition.restart_delays[2], std::chrono::seconds{60});
    EXPECT_EQ(api.last_definition.failure_reset, std::chrono::hours{24});
    EXPECT_EQ(api.last_definition.preshutdown_wait, std::chrono::seconds{30});
    EXPECT_TRUE(api.last_definition.restart_on_non_crash_failure);
}

TEST(WindowsScmManager, RestartsRunningAndStartsStoppedService)
{
    FakeServiceManagerApi api;
    api.presence = paperbreak::service::windows::ServicePresence::present;
    api.state = paperbreak::service::windows::ManagedServiceState::running;
    paperbreak::service::windows::ServiceManager manager{api};
    ASSERT_TRUE(manager.restart({}, std::chrono::milliseconds{10}));
    EXPECT_EQ(api.stop_calls, 1);
    EXPECT_EQ(api.start_calls, 1);

    api.state = paperbreak::service::windows::ManagedServiceState::stopped;
    ASSERT_TRUE(manager.restart({}, std::chrono::milliseconds{10}));
    EXPECT_EQ(api.stop_calls, 1);
    EXPECT_EQ(api.start_calls, 2);
}

TEST(WindowsScmManager, RestartReportsMissingAndStartFailure)
{
    FakeServiceManagerApi api;
    paperbreak::service::windows::ServiceManager manager{api};
    auto missing = manager.restart({}, std::chrono::milliseconds{0});
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().business_code, "SYS_SERVICE_RESTART_FAILED");

    api.presence = paperbreak::service::windows::ServicePresence::present;
    api.fail_start = true;
    auto failed = manager.restart({}, std::chrono::milliseconds{10});
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().business_code, "SYS_SERVICE_RESTART_FAILED");
}

TEST(WindowsScmManager, RestartIsBoundedAndCancellable)
{
    FakeServiceManagerApi api;
    api.presence = paperbreak::service::windows::ServicePresence::present;
    api.state = paperbreak::service::windows::ManagedServiceState::pending;
    api.stop_transitions = false;
    paperbreak::service::windows::ServiceManager manager{api};

    auto timeout = manager.restart({}, std::chrono::milliseconds{0});
    ASSERT_FALSE(timeout);
    EXPECT_EQ(timeout.error().business_code, "SYS_SERVICE_RESTART_FAILED");

    std::stop_source cancellation;
    cancellation.request_stop();
    auto cancelled = manager.restart({}, std::chrono::seconds{1}, cancellation.get_token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().business_code, "SYS_SERVICE_RESTART_CANCELLED");
}

TEST(WindowsScmManager, RepeatedInstallConvergesExistingConfiguration)
{
    FakeServiceManagerApi api;
    api.presence = paperbreak::service::windows::ServicePresence::present;
    paperbreak::service::windows::ServiceManager manager{api};

    auto result = manager.install({});

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value(), paperbreak::service::windows::InstallOutcome::converged);
    EXPECT_EQ(api.create_calls, 0);
    EXPECT_EQ(api.update_calls, 1);
    EXPECT_EQ(api.configure_calls, 1);
}

TEST(WindowsScmManager, RollsBackANewServiceWhenExtendedConfigurationFails)
{
    FakeServiceManagerApi api;
    api.fail_configure = true;
    paperbreak::service::windows::ServiceManager manager{api};

    auto result = manager.install({});

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().business_code, "SYS_SERVICE_INSTALL_FAILED");
    EXPECT_EQ(api.remove_calls, 1);
    ASSERT_FALSE(result.error().details.empty());
    EXPECT_EQ(result.error().details[0].value, "removed");
}

TEST(WindowsScmManager, PreservesNativeAccessDeniedDiagnostics)
{
    FakeServiceManagerApi api;
    api.fail_access = true;
    paperbreak::service::windows::ServiceManager manager{api};

    auto result = manager.install({});

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().native_domain, std::optional<std::string>{"win32"});
    EXPECT_EQ(result.error().native_code,
              std::optional<std::string>{std::to_string(ERROR_ACCESS_DENIED)});
    EXPECT_EQ(api.create_calls, 0);
}

TEST(WindowsScmManager, UninstallAlsoRequiresElevatedManagementAccess)
{
    FakeServiceManagerApi api;
    api.fail_access = true;
    paperbreak::service::windows::ServiceManager manager{api};

    auto result = manager.uninstall();

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().business_code, "SYS_SERVICE_UNINSTALL_FAILED");
    EXPECT_EQ(result.error().native_code,
              std::optional<std::string>{std::to_string(ERROR_ACCESS_DENIED)});
}

TEST(WindowsScmManager, UninstallIsIdempotentForMissingAndMarkedServices)
{
    FakeServiceManagerApi api;
    paperbreak::service::windows::ServiceManager manager{api};

    auto missing = manager.uninstall();
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing.value(), paperbreak::service::windows::UninstallOutcome::already_absent);

    api.presence = paperbreak::service::windows::ServicePresence::marked_for_delete;
    auto marked = manager.uninstall();
    ASSERT_TRUE(marked);
    EXPECT_EQ(marked.value(), paperbreak::service::windows::UninstallOutcome::already_absent);
    EXPECT_EQ(api.remove_calls, 0);
}

TEST(WindowsScmManager, StopsRunningServiceWithinTheSharedBoundBeforeDeleting)
{
    FakeServiceManagerApi api;
    api.presence = paperbreak::service::windows::ServicePresence::present;
    api.state = paperbreak::service::windows::ManagedServiceState::running;
    paperbreak::service::windows::ServiceManager manager{api};

    auto result = manager.uninstall();

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value(), paperbreak::service::windows::UninstallOutcome::removed);
    EXPECT_EQ(api.stop_calls, 1);
    EXPECT_EQ(api.wait_calls, 1);
    EXPECT_EQ(api.last_wait, std::chrono::seconds{30});
    EXPECT_EQ(api.remove_calls, 1);
}

TEST(WindowsScmManager, StopTimeoutKeepsTheServiceRegistered)
{
    FakeServiceManagerApi api;
    api.presence = paperbreak::service::windows::ServicePresence::present;
    api.state = paperbreak::service::windows::ManagedServiceState::pending;
    api.wait_succeeds = false;
    paperbreak::service::windows::ServiceManager manager{api};

    auto result = manager.uninstall();

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().business_code, "SYS_SERVICE_UNINSTALL_FAILED");
    EXPECT_EQ(api.remove_calls, 0);
}

TEST(WindowsScmCommandLine, QuotesUnicodeSpacesQuotesAndTrailingBackslashes)
{
    const std::filesystem::path executable{LR"(C:\Program Files\纸机\service.exe)"};
    const std::filesystem::path config{LR"(C:\配置 目录\edge.json)"};
    EXPECT_EQ(
        paperbreak::service::windows::build_service_command_line(executable, config),
        LR"("C:\Program Files\纸机\service.exe" --service --config "C:\配置 目录\edge.json")");
    EXPECT_EQ(paperbreak::service::windows::quote_windows_argument(LR"(C:\ends\)"),
              LR"("C:\ends\\")");
    EXPECT_EQ(paperbreak::service::windows::quote_windows_argument(LR"(a"b)"), LR"("a\"b")");
}

TEST(WindowsScmStatus, ReportsPendingPulsesRunningStopAndFailure)
{
    paperbreak::service::windows::ScmStatusModel model;
    const auto starting = model.begin_start(std::chrono::seconds{30});
    EXPECT_EQ(starting.state, paperbreak::service::windows::ScmState::start_pending);
    EXPECT_EQ(starting.checkpoint, 1U);
    EXPECT_EQ(model.pulse().checkpoint, 2U);

    const auto running = model.running();
    EXPECT_EQ(running.state, paperbreak::service::windows::ScmState::running);
    EXPECT_NE(running.controls_accepted & SERVICE_ACCEPT_STOP, 0U);
    EXPECT_NE(running.controls_accepted & SERVICE_ACCEPT_SHUTDOWN, 0U);
    EXPECT_NE(running.controls_accepted & SERVICE_ACCEPT_PRESHUTDOWN, 0U);

    const auto stopping = model.begin_stop(std::chrono::seconds{30});
    EXPECT_EQ(stopping.state, paperbreak::service::windows::ScmState::stop_pending);
    EXPECT_EQ(model.pulse().checkpoint, 2U);

    const auto stopped = model.stopped_success();
    EXPECT_EQ(stopped.state, paperbreak::service::windows::ScmState::stopped);
    EXPECT_EQ(stopped.win32_exit_code, NO_ERROR);
    const auto failed = model.stopped_failure(7U);
    EXPECT_EQ(failed.win32_exit_code, ERROR_SERVICE_SPECIFIC_ERROR);
    EXPECT_EQ(failed.service_specific_exit_code, 7U);
}

TEST(WindowsScmControl, MapsStopShutdownAndPreshutdownBehindAnExceptionBarrier)
{
    FakeHostedService service;
    EXPECT_TRUE(
        paperbreak::service::windows::dispatch_service_control(SERVICE_CONTROL_STOP, service));
    EXPECT_EQ(service.stop_reason, paperbreak::service::StopReason::service_stop);
    EXPECT_TRUE(
        paperbreak::service::windows::dispatch_service_control(SERVICE_CONTROL_SHUTDOWN, service));
    EXPECT_EQ(service.stop_reason, paperbreak::service::StopReason::system_shutdown);
    EXPECT_TRUE(paperbreak::service::windows::dispatch_service_control(SERVICE_CONTROL_PRESHUTDOWN,
                                                                       service));
    EXPECT_FALSE(
        paperbreak::service::windows::dispatch_service_control(SERVICE_CONTROL_PAUSE, service));

    service.throw_on_stop = true;
    EXPECT_FALSE(
        paperbreak::service::windows::dispatch_service_control(SERVICE_CONTROL_STOP, service));
}

TEST(WindowsScmControl, FirstStopReasonWinsInTheCapacityOneSlot)
{
    auto reason = paperbreak::service::windows::merge_stop_reason(
        std::nullopt, paperbreak::service::StopReason::system_shutdown);
    reason = paperbreak::service::windows::merge_stop_reason(
        reason, paperbreak::service::StopReason::service_stop);

    EXPECT_EQ(reason, paperbreak::service::StopReason::system_shutdown);
}
