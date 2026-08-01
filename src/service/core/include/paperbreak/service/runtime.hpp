#pragma once

#include "paperbreak/common/result.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string_view>
#include <vector>

namespace paperbreak::service
{

enum class ServiceState
{
    created,
    starting,
    running,
    stop_requested,
    draining,
    stopped,
    failed,
};

enum class StartOutcome
{
    running,
    cancelled,
};

enum class StopReason
{
    console_interrupt,
    service_stop,
    system_shutdown,
    test_deadline,
};

enum class ShutdownPhase
{
    configuration,
    acquisition,
    processing,
    event,
    uplink,
    monitoring,
    ipc,
    logging,
};

struct RuntimeOptions final
{
    std::chrono::milliseconds shutdown_timeout{std::chrono::seconds{30}};
};

/// One injected service component. request_stop must be non-blocking; join must honor deadline.
class ILifecycleComponent
{
  public:
    virtual ~ILifecycleComponent() = default;

    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual ShutdownPhase shutdown_phase() const noexcept = 0;
    [[nodiscard]] virtual Result<void> start(std::stop_token startup_stop_token) = 0;
    [[nodiscard]] virtual Result<void> request_stop(StopReason reason) = 0;
    [[nodiscard]] virtual Result<void> join(std::chrono::steady_clock::time_point deadline) = 0;
};

/// Owns the injected components and provides the single host-neutral lifecycle implementation.
class ServiceRuntime final
{
  public:
    explicit ServiceRuntime(std::vector<std::unique_ptr<ILifecycleComponent>> components,
                            RuntimeOptions options = {});
    ~ServiceRuntime();

    ServiceRuntime(const ServiceRuntime&) = delete;
    ServiceRuntime& operator=(const ServiceRuntime&) = delete;
    ServiceRuntime(ServiceRuntime&&) = delete;
    ServiceRuntime& operator=(ServiceRuntime&&) = delete;

    [[nodiscard]] Result<StartOutcome> start();
    void request_stop(StopReason reason) noexcept;
    [[nodiscard]] Result<void> shutdown();

    [[nodiscard]] ServiceState state() const noexcept;
    [[nodiscard]] std::optional<StopReason> stop_reason() const noexcept;

  private:
    [[nodiscard]] std::optional<Error> rollback_started(
        std::chrono::steady_clock::time_point deadline);
    [[nodiscard]] std::vector<std::size_t> shutdown_order() const;

    std::vector<std::unique_ptr<ILifecycleComponent>> components_;
    RuntimeOptions options_;
    std::atomic<ServiceState> state_{ServiceState::created};
    std::atomic<int> stop_reason_value_{-1};
    std::stop_source startup_stop_source_;
    std::mutex operation_mutex_;
    std::vector<std::size_t> started_indices_;
    std::optional<Error> terminal_error_;
};

[[nodiscard]] std::string_view service_state_name(ServiceState state) noexcept;
[[nodiscard]] std::string_view shutdown_phase_name(ShutdownPhase phase) noexcept;

} // namespace paperbreak::service
