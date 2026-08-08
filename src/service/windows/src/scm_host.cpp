#include "paperbreak/service/windows/scm_host.hpp"
#include "paperbreak/service/windows/scm.hpp"

#include <Windows.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>

namespace paperbreak::service::windows
{

namespace
{

constexpr std::uint32_t service_specific_start_failure = 1;
constexpr std::uint32_t service_specific_stop_failure = 2;

Error host_error(std::string message, std::string operation, const DWORD native_code)
{
    Error error = make_error("SYS_SERVICE_CONTROL_FAILED", Severity::critical, std::move(message),
                             "service", std::move(operation));
    error.native_domain = "win32";
    error.native_code = std::to_string(native_code);
    return error;
}

std::optional<StopReason> map_service_control(const std::uint32_t control_code) noexcept
{
    switch (control_code)
    {
    case SERVICE_CONTROL_STOP:
        return StopReason::service_stop;
    case SERVICE_CONTROL_SHUTDOWN:
    case SERVICE_CONTROL_PRESHUTDOWN:
        return StopReason::system_shutdown;
    default:
        return std::nullopt;
    }
}

class StopRequestSlot final
{
  public:
    void request(const StopReason reason) noexcept
    {
        try
        {
            {
                std::scoped_lock lock{mutex_};
                if (!reason_.has_value())
                {
                    reason_ = merge_stop_reason(reason_, reason);
                }
            }
            condition_.notify_one();
        }
        catch (...)
        {
        }
    }

    [[nodiscard]] StopReason wait()
    {
        std::unique_lock lock{mutex_};
        condition_.wait(lock, [this] { return reason_.has_value(); });
        return reason_.value();
    }

    [[nodiscard]] std::optional<StopReason> current() const
    {
        std::scoped_lock lock{mutex_};
        return reason_;
    }

  private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<StopReason> reason_;
};

class StatusReporter final
{
  public:
    explicit StatusReporter(const SERVICE_STATUS_HANDLE handle) noexcept : handle_(handle) {}

    [[nodiscard]] bool report(const ScmStatus& source) noexcept
    {
        std::scoped_lock lock{mutex_};
        SERVICE_STATUS status{};
        status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
        switch (source.state)
        {
        case ScmState::start_pending:
            status.dwCurrentState = SERVICE_START_PENDING;
            break;
        case ScmState::running:
            status.dwCurrentState = SERVICE_RUNNING;
            break;
        case ScmState::stop_pending:
            status.dwCurrentState = SERVICE_STOP_PENDING;
            break;
        case ScmState::stopped:
            status.dwCurrentState = SERVICE_STOPPED;
            break;
        }
        status.dwControlsAccepted = source.controls_accepted;
        status.dwWin32ExitCode = source.win32_exit_code;
        status.dwServiceSpecificExitCode = source.service_specific_exit_code;
        status.dwCheckPoint = source.checkpoint;
        status.dwWaitHint = static_cast<DWORD>(source.wait_hint.count());
        if (SetServiceStatus(handle_, &status) != FALSE)
        {
            return true;
        }
        if (!error_.has_value())
        {
            error_ = host_error("无法向 SCM 上报服务状态", "service.scmHost.reportStatus",
                                GetLastError());
        }
        return false;
    }

    [[nodiscard]] std::optional<Error> error() const
    {
        std::scoped_lock lock{mutex_};
        return error_;
    }

  private:
    SERVICE_STATUS_HANDLE handle_{nullptr};
    mutable std::mutex mutex_;
    std::optional<Error> error_;
};

class PendingStatusPulse final
{
  public:
    PendingStatusPulse(ScmStatusModel& model, StatusReporter& reporter)
        : model_(model), reporter_(reporter), thread_([this](const std::stop_token token) {
              static_cast<void>(
                  ::SetThreadDescription(::GetCurrentThread(), L"service-scm-status"));
              std::unique_lock lock{mutex_};
              while (!token.stop_requested())
              {
                  static_cast<void>(condition_.wait_for(lock, token, std::chrono::seconds{1},
                                                        [] { return false; }));
                  if (!token.stop_requested())
                  {
                      lock.unlock();
                      static_cast<void>(reporter_.report(model_.pulse()));
                      lock.lock();
                  }
              }
          })
    {
    }

    ~PendingStatusPulse()
    {
        stop();
    }

    PendingStatusPulse(const PendingStatusPulse&) = delete;
    PendingStatusPulse& operator=(const PendingStatusPulse&) = delete;

    void stop() noexcept
    {
        if (thread_.joinable())
        {
            static_cast<void>(thread_.request_stop());
            condition_.notify_all();
            thread_.join();
        }
    }

  private:
    ScmStatusModel& model_;
    StatusReporter& reporter_;
    std::mutex mutex_;
    std::condition_variable_any condition_;
    std::jthread thread_;
};

struct DispatcherState final
{
    explicit DispatcherState(HostedServiceFactory value) : factory(std::move(value)) {}

    void set_error(Error value)
    {
        std::scoped_lock lock{mutex};
        if (!error.has_value())
        {
            error = std::move(value);
        }
    }

    [[nodiscard]] std::optional<Error> get_error() const
    {
        std::scoped_lock lock{mutex};
        return error;
    }

    HostedServiceFactory factory;
    mutable std::mutex mutex;
    std::optional<Error> error;
};

struct HostExecution final
{
    std::atomic<std::shared_ptr<IHostedService>> service;
    StopRequestSlot stop_slot;
};

std::atomic<std::shared_ptr<DispatcherState>> active_dispatcher;

DWORD WINAPI service_control_handler(const DWORD control_code, DWORD, LPVOID,
                                     LPVOID context) noexcept
{
    try
    {
        auto* execution = static_cast<HostExecution*>(context);
        if (execution == nullptr)
        {
            return ERROR_INVALID_PARAMETER;
        }
        if (control_code == SERVICE_CONTROL_INTERROGATE)
        {
            return NO_ERROR;
        }
        const auto reason = map_service_control(control_code);
        if (!reason.has_value())
        {
            return ERROR_CALL_NOT_IMPLEMENTED;
        }
        execution->stop_slot.request(reason.value());
        if (const auto service = execution->service.load(std::memory_order_acquire); service)
        {
            service->request_stop(reason.value());
        }
        return NO_ERROR;
    }
    catch (...)
    {
        return ERROR_EXCEPTION_IN_SERVICE;
    }
}

void store_reporter_error(const std::shared_ptr<DispatcherState>& dispatcher,
                          const StatusReporter& reporter)
{
    if (auto status_error = reporter.error(); status_error.has_value())
    {
        dispatcher->set_error(std::move(status_error).value());
    }
}

VOID WINAPI service_main(DWORD, LPWSTR*) noexcept
{
    const auto dispatcher = active_dispatcher.load(std::memory_order_acquire);
    if (!dispatcher)
    {
        return;
    }

    try
    {
        HostExecution execution;
        const SERVICE_STATUS_HANDLE status_handle = RegisterServiceCtrlHandlerExW(
            L"PaperBreakEdgeService", service_control_handler, &execution);
        if (status_handle == nullptr)
        {
            dispatcher->set_error(host_error(
                "无法注册 SCM 控制回调", "service.scmHost.registerControlHandler", GetLastError()));
            return;
        }

        ScmStatusModel status_model;
        StatusReporter reporter{status_handle};
        if (!reporter.report(status_model.begin_start(service_stop_timeout)))
        {
            store_reporter_error(dispatcher, reporter);
            return;
        }
        PendingStatusPulse start_pulse{status_model, reporter};

        auto service_result = [&dispatcher]() -> Result<std::unique_ptr<IHostedService>> {
            try
            {
                return dispatcher->factory();
            }
            catch (...)
            {
                return Result<std::unique_ptr<IHostedService>>::failure(make_error(
                    "SYS_SERVICE_CONTROL_FAILED", Severity::critical,
                    "SCM 服务工厂捕获到未处理异常", "service", "service.scmHost.createService"));
            }
        }();
        if (!service_result)
        {
            start_pulse.stop();
            dispatcher->set_error(service_result.error());
            static_cast<void>(
                reporter.report(status_model.stopped_failure(service_specific_start_failure)));
            store_reporter_error(dispatcher, reporter);
            return;
        }

        std::shared_ptr<IHostedService> service = std::move(service_result).value();
        execution.service.store(service, std::memory_order_release);
        if (const auto pending_reason = execution.stop_slot.current(); pending_reason.has_value())
        {
            service->request_stop(pending_reason.value());
        }

        auto start_result = [&service]() -> Result<StartOutcome> {
            try
            {
                return service->start();
            }
            catch (...)
            {
                return Result<StartOutcome>::failure(make_error(
                    "SYS_SERVICE_START_FAILED", Severity::critical,
                    "SCM 服务启动边界捕获到未处理异常", "service", "service.scmHost.start"));
            }
        }();
        start_pulse.stop();
        if (!start_result)
        {
            execution.service.store({}, std::memory_order_release);
            dispatcher->set_error(start_result.error());
            static_cast<void>(
                reporter.report(status_model.stopped_failure(service_specific_start_failure)));
            store_reporter_error(dispatcher, reporter);
            return;
        }

        if (const auto status_error = reporter.error(); status_error.has_value())
        {
            dispatcher->set_error(status_error.value());
            execution.stop_slot.request(StopReason::service_stop);
            service->request_stop(StopReason::service_stop);
        }

        if (start_result.value() == StartOutcome::running)
        {
            if (!reporter.report(status_model.running()))
            {
                execution.stop_slot.request(StopReason::service_stop);
                service->request_stop(StopReason::service_stop);
            }
        }

        StopReason reason = StopReason::service_stop;
        if (const auto pending_reason = execution.stop_slot.current(); pending_reason.has_value())
        {
            reason = pending_reason.value();
        }
        else if (start_result.value() == StartOutcome::running)
        {
            reason = execution.stop_slot.wait();
        }
        service->request_stop(reason);
        static_cast<void>(reporter.report(status_model.begin_stop(service_stop_timeout)));
        PendingStatusPulse stop_pulse{status_model, reporter};
        auto shutdown_result = [&service]() -> Result<void> {
            try
            {
                return service->shutdown();
            }
            catch (...)
            {
                return Result<void>::failure(make_error(
                    "SYS_SERVICE_CONTROL_FAILED", Severity::critical,
                    "SCM 服务关闭边界捕获到未处理异常", "service", "service.scmHost.shutdown"));
            }
        }();
        stop_pulse.stop();
        execution.service.store({}, std::memory_order_release);

        if (!shutdown_result)
        {
            dispatcher->set_error(shutdown_result.error());
            static_cast<void>(
                reporter.report(status_model.stopped_failure(service_specific_stop_failure)));
        }
        else
        {
            static_cast<void>(reporter.report(status_model.stopped_success()));
        }
        store_reporter_error(dispatcher, reporter);
    }
    catch (...)
    {
        dispatcher->set_error(make_error("SYS_SERVICE_CONTROL_FAILED", Severity::critical,
                                         "SCM ServiceMain 捕获到未处理异常", "service",
                                         "service.scmHost.serviceMain"));
    }
}

} // namespace

ScmStatus ScmStatusModel::begin_start(const std::chrono::milliseconds wait_hint) noexcept
{
    status_ = {.state = ScmState::start_pending,
               .controls_accepted = 0,
               .checkpoint = 1,
               .wait_hint = wait_hint,
               .win32_exit_code = NO_ERROR,
               .service_specific_exit_code = 0};
    return status_;
}

ScmStatus ScmStatusModel::pulse() noexcept
{
    if (status_.state == ScmState::start_pending || status_.state == ScmState::stop_pending)
    {
        ++status_.checkpoint;
    }
    return status_;
}

ScmStatus ScmStatusModel::running() noexcept
{
    status_ = {.state = ScmState::running,
               .controls_accepted =
                   SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_PRESHUTDOWN,
               .checkpoint = 0,
               .wait_hint = std::chrono::milliseconds{0},
               .win32_exit_code = NO_ERROR,
               .service_specific_exit_code = 0};
    return status_;
}

ScmStatus ScmStatusModel::begin_stop(const std::chrono::milliseconds wait_hint) noexcept
{
    status_ = {.state = ScmState::stop_pending,
               .controls_accepted = 0,
               .checkpoint = 1,
               .wait_hint = wait_hint,
               .win32_exit_code = NO_ERROR,
               .service_specific_exit_code = 0};
    return status_;
}

ScmStatus ScmStatusModel::stopped_success() noexcept
{
    status_ = {.state = ScmState::stopped,
               .controls_accepted = 0,
               .checkpoint = 0,
               .wait_hint = std::chrono::milliseconds{0},
               .win32_exit_code = NO_ERROR,
               .service_specific_exit_code = 0};
    return status_;
}

ScmStatus ScmStatusModel::stopped_failure(const std::uint32_t service_specific_code) noexcept
{
    status_ = {.state = ScmState::stopped,
               .controls_accepted = 0,
               .checkpoint = 0,
               .wait_hint = std::chrono::milliseconds{0},
               .win32_exit_code = ERROR_SERVICE_SPECIFIC_ERROR,
               .service_specific_exit_code = service_specific_code};
    return status_;
}

bool dispatch_service_control(const std::uint32_t control_code, IHostedService& service) noexcept
{
    const auto reason = map_service_control(control_code);
    if (!reason.has_value())
    {
        return false;
    }
    try
    {
        service.request_stop(reason.value());
        return true;
    }
    catch (...)
    {
        return false;
    }
}

std::optional<StopReason> merge_stop_reason(std::optional<StopReason> current,
                                            const StopReason incoming) noexcept
{
    if (!current.has_value())
    {
        current = incoming;
    }
    return current;
}

Result<void> run_service_dispatcher(HostedServiceFactory factory)
{
    if (!factory)
    {
        return Result<void>::failure(make_error("SYS_SERVICE_CONTROL_FAILED", Severity::critical,
                                                "SCM 服务工厂为空", "service",
                                                "service.scmHost.dispatch"));
    }

    auto dispatcher = std::make_shared<DispatcherState>(std::move(factory));
    std::shared_ptr<DispatcherState> expected;
    if (!active_dispatcher.compare_exchange_strong(expected, dispatcher, std::memory_order_acq_rel))
    {
        return Result<void>::failure(make_error("SYS_SERVICE_CONTROL_FAILED", Severity::critical,
                                                "SCM 调度器已经运行", "service",
                                                "service.scmHost.dispatch"));
    }

    SERVICE_TABLE_ENTRYW dispatch_table[]{
        {const_cast<LPWSTR>(L"PaperBreakEdgeService"), service_main}, {nullptr, nullptr}};
    const BOOL dispatch_result = StartServiceCtrlDispatcherW(dispatch_table);
    const DWORD native_code = dispatch_result == FALSE ? GetLastError() : NO_ERROR;
    auto active = dispatcher;
    static_cast<void>(
        active_dispatcher.compare_exchange_strong(active, {}, std::memory_order_acq_rel));

    if (dispatch_result == FALSE)
    {
        return Result<void>::failure(
            host_error("无法进入 Windows 服务调度器", "service.scmHost.dispatch", native_code));
    }
    if (auto error = dispatcher->get_error(); error.has_value())
    {
        return Result<void>::failure(std::move(error).value());
    }
    return Result<void>::success();
}

} // namespace paperbreak::service::windows
