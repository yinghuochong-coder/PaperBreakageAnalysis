#include "paperbreak/service/runtime.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <string>
#include <utility>

namespace paperbreak::service
{
namespace
{

Error component_exception(const ILifecycleComponent& component, const std::string_view operation)
{
    Error error = make_error("SYS_INTERNAL_ERROR", Severity::error, "生命周期组件抛出了未处理异常",
                             "service", "service.componentException");
    error.details.push_back({"component", std::string{component.name()}});
    error.details.push_back(
        {"phase", std::string{shutdown_phase_name(component.shutdown_phase())}});
    error.details.push_back({"operation", std::string{operation}});
    return error;
}

Result<void> request_component_stop(ILifecycleComponent& component, const StopReason reason)
{
    try
    {
        return component.request_stop(reason);
    }
    catch (...)
    {
        return Result<void>::failure(component_exception(component, "requestStop"));
    }
}

Result<void> join_component(ILifecycleComponent& component,
                            const std::chrono::steady_clock::time_point deadline)
{
    try
    {
        return component.join(deadline);
    }
    catch (...)
    {
        return Result<void>::failure(component_exception(component, "join"));
    }
}

Error shutdown_timeout(const ILifecycleComponent& component)
{
    Error error =
        make_error("SYS_SHUTDOWN_TIMEOUT", Severity::critical, "组件未在共享关闭截止时间内停止",
                   "service", "service.shutdown", false);
    error.details.push_back({"component", std::string{component.name()}});
    error.details.push_back(
        {"phase", std::string{shutdown_phase_name(component.shutdown_phase())}});
    return error;
}

Error startup_failure(const ILifecycleComponent& component, const Error& cause)
{
    Error error = make_error("SYS_SERVICE_START_FAILED", Severity::critical, "必需服务组件启动失败",
                             "service", "service.start", cause.retryable);
    error.details.push_back({"component", std::string{component.name()}});
    error.details.push_back(
        {"phase", std::string{shutdown_phase_name(component.shutdown_phase())}});
    error.details.push_back({"causeBusinessCode", cause.business_code});
    if (cause.native_domain.has_value())
    {
        error.native_domain = cause.native_domain;
    }
    if (cause.native_code.has_value())
    {
        error.native_code = cause.native_code;
    }
    return error;
}

} // namespace

ServiceRuntime::ServiceRuntime(std::vector<std::unique_ptr<ILifecycleComponent>> components,
                               RuntimeOptions options)
    : components_(std::move(components)), options_(options)
{
}

ServiceRuntime::~ServiceRuntime()
{
    static_cast<void>(shutdown());
}

Result<StartOutcome> ServiceRuntime::start()
{
    std::scoped_lock lock{operation_mutex_};

    const ServiceState current = state_.load(std::memory_order_acquire);
    if (current == ServiceState::running)
    {
        return Result<StartOutcome>::success(StartOutcome::running);
    }
    if (current != ServiceState::created)
    {
        Error error = make_error("SYS_INTERNAL_ERROR", Severity::error,
                                 "服务运行时不能从当前状态启动", "service", "service.start");
        error.details.push_back({"state", std::string{service_state_name(current)}});
        return Result<StartOutcome>::failure(std::move(error));
    }

    state_.store(ServiceState::starting, std::memory_order_release);
    started_indices_.clear();

    for (std::size_t index = 0; index < components_.size(); ++index)
    {
        if (startup_stop_source_.stop_requested())
        {
            const auto rollback_error =
                rollback_started(std::chrono::steady_clock::now() + options_.shutdown_timeout);
            if (rollback_error.has_value())
            {
                state_.store(ServiceState::failed, std::memory_order_release);
                terminal_error_ = rollback_error;
                return Result<StartOutcome>::failure(rollback_error.value());
            }
            state_.store(ServiceState::stopped, std::memory_order_release);
            return Result<StartOutcome>::success(StartOutcome::cancelled);
        }

        Result<void> component_result = Result<void>::success();
        try
        {
            component_result = components_[index]->start(startup_stop_source_.get_token());
        }
        catch (...)
        {
            component_result =
                Result<void>::failure(component_exception(*components_[index], "start"));
        }

        if (startup_stop_source_.stop_requested())
        {
            if (component_result)
            {
                started_indices_.push_back(index);
            }
            const auto rollback_error =
                rollback_started(std::chrono::steady_clock::now() + options_.shutdown_timeout);
            if (rollback_error.has_value())
            {
                terminal_error_ = rollback_error;
                state_.store(ServiceState::failed, std::memory_order_release);
                return Result<StartOutcome>::failure(rollback_error.value());
            }
            state_.store(ServiceState::stopped, std::memory_order_release);
            return Result<StartOutcome>::success(StartOutcome::cancelled);
        }

        if (!component_result)
        {
            Error error = startup_failure(*components_[index], component_result.error());
            const auto rollback_error =
                rollback_started(std::chrono::steady_clock::now() + options_.shutdown_timeout);
            if (rollback_error.has_value())
            {
                error.details.push_back({"rollbackBusinessCode", rollback_error->business_code});
            }
            terminal_error_ = error;
            state_.store(ServiceState::failed, std::memory_order_release);
            return Result<StartOutcome>::failure(std::move(error));
        }

        started_indices_.push_back(index);
    }

    ServiceState expected = ServiceState::starting;
    if (!state_.compare_exchange_strong(expected, ServiceState::running, std::memory_order_acq_rel))
    {
        const auto rollback_error =
            rollback_started(std::chrono::steady_clock::now() + options_.shutdown_timeout);
        if (rollback_error.has_value())
        {
            terminal_error_ = rollback_error;
            state_.store(ServiceState::failed, std::memory_order_release);
            return Result<StartOutcome>::failure(rollback_error.value());
        }
        state_.store(ServiceState::stopped, std::memory_order_release);
        return Result<StartOutcome>::success(StartOutcome::cancelled);
    }
    return Result<StartOutcome>::success(StartOutcome::running);
}

void ServiceRuntime::request_stop(const StopReason reason) noexcept
{
    int expected = -1;
    static_cast<void>(stop_reason_value_.compare_exchange_strong(expected, static_cast<int>(reason),
                                                                 std::memory_order_acq_rel));
    static_cast<void>(startup_stop_source_.request_stop());

    ServiceState current = state_.load(std::memory_order_acquire);
    while (current == ServiceState::starting || current == ServiceState::running)
    {
        if (state_.compare_exchange_weak(current, ServiceState::stop_requested,
                                         std::memory_order_acq_rel))
        {
            break;
        }
    }
}

Result<void> ServiceRuntime::shutdown()
{
    std::scoped_lock lock{operation_mutex_};
    const ServiceState current = state_.load(std::memory_order_acquire);
    if (current == ServiceState::stopped)
    {
        return Result<void>::success();
    }
    if (current == ServiceState::failed)
    {
        if (terminal_error_.has_value())
        {
            return Result<void>::failure(terminal_error_.value());
        }
        return Result<void>::failure(make_error("SYS_INTERNAL_ERROR", Severity::error,
                                                "服务运行时处于失败状态", "service",
                                                "service.shutdown"));
    }
    if (current == ServiceState::created)
    {
        state_.store(ServiceState::stopped, std::memory_order_release);
        return Result<void>::success();
    }
    if (current == ServiceState::starting)
    {
        return Result<void>::failure(make_error("SYS_SERVICE_STOPPING", Severity::warning,
                                                "启动尚未返回，只允许先请求停止", "service",
                                                "service.shutdown", true));
    }

    if (!stop_reason().has_value())
    {
        request_stop(StopReason::service_stop);
    }
    state_.store(ServiceState::draining, std::memory_order_release);

    const auto deadline = std::chrono::steady_clock::now() + options_.shutdown_timeout;
    const StopReason reason = stop_reason().value_or(StopReason::service_stop);
    const auto ordered_indices = shutdown_order();
    std::optional<Error> first_error;

    for (const std::size_t index : ordered_indices)
    {
        auto stop_result = request_component_stop(*components_[index], reason);
        if (!stop_result && !first_error.has_value())
        {
            first_error = stop_result.error();
        }
    }

    for (const std::size_t index : ordered_indices)
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            first_error = shutdown_timeout(*components_[index]);
            break;
        }

        auto join_result = join_component(*components_[index], deadline);
        if (std::chrono::steady_clock::now() > deadline)
        {
            first_error = shutdown_timeout(*components_[index]);
            break;
        }
        if (!join_result && join_result.error().business_code == "SYS_SHUTDOWN_TIMEOUT")
        {
            first_error = shutdown_timeout(*components_[index]);
            break;
        }
        if (!join_result && !first_error.has_value())
        {
            first_error = join_result.error();
        }
    }

    if (first_error.has_value())
    {
        terminal_error_ = first_error;
        state_.store(ServiceState::failed, std::memory_order_release);
        return Result<void>::failure(first_error.value());
    }

    state_.store(ServiceState::stopped, std::memory_order_release);
    return Result<void>::success();
}

ServiceState ServiceRuntime::state() const noexcept
{
    return state_.load(std::memory_order_acquire);
}

std::optional<StopReason> ServiceRuntime::stop_reason() const noexcept
{
    const int value = stop_reason_value_.load(std::memory_order_acquire);
    if (value < 0)
    {
        return std::nullopt;
    }
    return static_cast<StopReason>(value);
}

std::optional<Error> ServiceRuntime::rollback_started(
    const std::chrono::steady_clock::time_point deadline)
{
    std::optional<Error> first_error;
    const StopReason reason = stop_reason().value_or(StopReason::service_stop);

    for (auto iterator = started_indices_.rbegin(); iterator != started_indices_.rend(); ++iterator)
    {
        auto stop_result = request_component_stop(*components_[*iterator], reason);
        if (!stop_result && !first_error.has_value())
        {
            first_error = stop_result.error();
        }
    }
    for (auto iterator = started_indices_.rbegin(); iterator != started_indices_.rend(); ++iterator)
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return shutdown_timeout(*components_[*iterator]);
        }
        auto join_result = join_component(*components_[*iterator], deadline);
        if (std::chrono::steady_clock::now() > deadline)
        {
            return shutdown_timeout(*components_[*iterator]);
        }
        if (!join_result && join_result.error().business_code == "SYS_SHUTDOWN_TIMEOUT")
        {
            return shutdown_timeout(*components_[*iterator]);
        }
        if (!join_result && !first_error.has_value())
        {
            first_error = join_result.error();
        }
    }
    started_indices_.clear();
    return first_error;
}

std::vector<std::size_t> ServiceRuntime::shutdown_order() const
{
    constexpr std::array phases{ShutdownPhase::configuration, ShutdownPhase::acquisition,
                                ShutdownPhase::processing,    ShutdownPhase::event,
                                ShutdownPhase::uplink,        ShutdownPhase::ipc,
                                ShutdownPhase::logging};
    std::vector<std::size_t> ordered;
    ordered.reserve(started_indices_.size());
    for (const ShutdownPhase phase : phases)
    {
        for (auto iterator = started_indices_.rbegin(); iterator != started_indices_.rend();
             ++iterator)
        {
            if (components_[*iterator]->shutdown_phase() == phase)
            {
                ordered.push_back(*iterator);
            }
        }
    }
    return ordered;
}

std::string_view service_state_name(const ServiceState state) noexcept
{
    switch (state)
    {
    case ServiceState::created:
        return "created";
    case ServiceState::starting:
        return "starting";
    case ServiceState::running:
        return "running";
    case ServiceState::stop_requested:
        return "stop-requested";
    case ServiceState::draining:
        return "draining";
    case ServiceState::stopped:
        return "stopped";
    case ServiceState::failed:
        return "failed";
    }
    return "unknown";
}

std::string_view shutdown_phase_name(const ShutdownPhase phase) noexcept
{
    switch (phase)
    {
    case ShutdownPhase::configuration:
        return "configuration";
    case ShutdownPhase::acquisition:
        return "acquisition";
    case ShutdownPhase::processing:
        return "processing";
    case ShutdownPhase::event:
        return "event";
    case ShutdownPhase::uplink:
        return "uplink";
    case ShutdownPhase::ipc:
        return "ipc";
    case ShutdownPhase::logging:
        return "logging";
    }
    return "unknown";
}

} // namespace paperbreak::service
