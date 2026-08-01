#pragma once

#include "paperbreak/common/result.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace paperbreak::platform
{

struct LocalIpcPeerInfo final
{
    std::string actor_sid;
    bool local{};
    bool authenticated{};
    bool administrator{};
};

/// Inspects a Windows named-pipe server handle after at least one client message was read.
[[nodiscard]] Result<LocalIpcPeerInfo> inspect_local_named_pipe_peer(
    std::uintptr_t native_handle) noexcept;

/// Cross-session, process-independent guard for the single IPC server instance.
class NamedInstanceGuard final
{
  public:
    explicit NamedInstanceGuard(void* handle) noexcept;
    ~NamedInstanceGuard();

    NamedInstanceGuard(const NamedInstanceGuard&) = delete;
    NamedInstanceGuard& operator=(const NamedInstanceGuard&) = delete;
    NamedInstanceGuard(NamedInstanceGuard&&) noexcept;
    NamedInstanceGuard& operator=(NamedInstanceGuard&&) noexcept;

    [[nodiscard]] static Result<std::unique_ptr<NamedInstanceGuard>> acquire(
        const std::wstring& name) noexcept;

  private:
    void* handle_{};
};

} // namespace paperbreak::platform
