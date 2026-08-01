#pragma once

#include "paperbreak/common/result.hpp"
#include "paperbreak/service/runtime.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace paperbreak::service::windows
{

enum class ScmState
{
    start_pending,
    running,
    stop_pending,
    stopped,
};

struct ScmStatus final
{
    ScmState state{ScmState::stopped};
    std::uint32_t controls_accepted{0};
    std::uint32_t checkpoint{0};
    std::chrono::milliseconds wait_hint{0};
    std::uint32_t win32_exit_code{0};
    std::uint32_t service_specific_exit_code{0};
};

class ScmStatusModel final
{
  public:
    [[nodiscard]] ScmStatus begin_start(std::chrono::milliseconds wait_hint) noexcept;
    [[nodiscard]] ScmStatus pulse() noexcept;
    [[nodiscard]] ScmStatus running() noexcept;
    [[nodiscard]] ScmStatus begin_stop(std::chrono::milliseconds wait_hint) noexcept;
    [[nodiscard]] ScmStatus stopped_success() noexcept;
    [[nodiscard]] ScmStatus stopped_failure(std::uint32_t service_specific_code) noexcept;

  private:
    ScmStatus status_{};
};

class IHostedService
{
  public:
    virtual ~IHostedService() = default;
    [[nodiscard]] virtual Result<StartOutcome> start() = 0;
    virtual void request_stop(StopReason reason) = 0;
    [[nodiscard]] virtual Result<void> shutdown() = 0;
};

using HostedServiceFactory = std::function<Result<std::unique_ptr<IHostedService>>()>;

/// Preserves the first stop reason in the process-scoped capacity-one request slot.
[[nodiscard]] std::optional<StopReason> merge_stop_reason(std::optional<StopReason> current,
                                                          StopReason incoming) noexcept;

/// Maps and dispatches one raw SCM control code behind an exception barrier.
[[nodiscard]] bool dispatch_service_control(std::uint32_t control_code,
                                            IHostedService& service) noexcept;

/// Enters the Windows service dispatcher and blocks until ServiceMain has stopped.
[[nodiscard]] Result<void> run_service_dispatcher(HostedServiceFactory factory);

} // namespace paperbreak::service::windows
