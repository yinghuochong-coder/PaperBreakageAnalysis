#include "paperbreak/platform/local_ipc_security.hpp"

#include <Windows.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <string>
#include <utility>
#include <vector>

namespace paperbreak::platform
{
namespace
{

Error win32_error(std::string business_code, std::string message, std::string operation,
                  const DWORD native_code)
{
    Error error = make_error(std::move(business_code), Severity::error, std::move(message), "ipc",
                             std::move(operation));
    error.native_domain = "win32";
    error.native_code = std::to_string(native_code);
    return error;
}

class RevertGuard final
{
  public:
    ~RevertGuard()
    {
        if (active_)
        {
            static_cast<void>(RevertToSelf());
        }
    }

    void release() noexcept
    {
        active_ = false;
    }

  private:
    bool active_{true};
};

bool equal_case_insensitive(const std::wstring& left, const std::wstring& right)
{
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(),
                      [](const wchar_t a, const wchar_t b) {
                          return std::towlower(a) == std::towlower(b);
                      });
}

Result<std::wstring> client_computer_name(const HANDLE pipe)
{
    std::array<wchar_t, MAX_COMPUTERNAME_LENGTH + 1U> buffer{};
    ULONG size = static_cast<ULONG>(buffer.size());
    if (GetNamedPipeClientComputerNameW(pipe, buffer.data(), size) == FALSE)
    {
        if (GetLastError() == ERROR_PIPE_LOCAL)
        {
            return Result<std::wstring>::success(L".");
        }
        return Result<std::wstring>::failure(win32_error(
            "IPC_UNAUTHORIZED", "无法确认命名管道客户端来源", "ipc.peerComputer", GetLastError()));
    }
    return Result<std::wstring>::success(std::wstring{buffer.data()});
}

Result<std::wstring> local_computer_name()
{
    std::array<wchar_t, MAX_COMPUTERNAME_LENGTH + 1U> buffer{};
    DWORD size = static_cast<DWORD>(buffer.size());
    if (GetComputerNameW(buffer.data(), &size) == FALSE)
    {
        return Result<std::wstring>::failure(win32_error("IPC_UNAUTHORIZED", "无法确认本机名称",
                                                         "ipc.localComputer", GetLastError()));
    }
    return Result<std::wstring>::success(std::wstring{buffer.data(), size});
}

Result<std::string> current_token_sid(const HANDLE token)
{
    DWORD size = 0;
    static_cast<void>(GetTokenInformation(token, TokenUser, nullptr, 0, &size));
    if (size == 0)
    {
        return Result<std::string>::failure(win32_error(
            "IPC_UNAUTHORIZED", "无法读取客户端令牌大小", "ipc.peerToken", GetLastError()));
    }
    std::vector<std::byte> storage(size);
    if (GetTokenInformation(token, TokenUser, storage.data(), size, &size) == FALSE)
    {
        return Result<std::string>::failure(win32_error(
            "IPC_UNAUTHORIZED", "无法读取客户端用户 SID", "ipc.peerToken", GetLastError()));
    }
    const auto* token_user = reinterpret_cast<const TOKEN_USER*>(storage.data());
    LPWSTR sid_text = nullptr;
    if (ConvertSidToStringSidW(token_user->User.Sid, &sid_text) == FALSE)
    {
        return Result<std::string>::failure(win32_error(
            "IPC_UNAUTHORIZED", "无法格式化客户端用户 SID", "ipc.peerSid", GetLastError()));
    }
    const std::wstring wide{sid_text};
    LocalFree(sid_text);
    std::string result;
    result.reserve(wide.size());
    for (const wchar_t character : wide)
    {
        result.push_back(static_cast<char>(character));
    }
    return Result<std::string>::success(std::move(result));
}

Result<bool> current_token_is_administrator()
{
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID administrators = nullptr;
    if (AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                 DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
                                 &administrators) == FALSE)
    {
        return Result<bool>::failure(
            win32_error("IPC_UNAUTHORIZED", "无法构造管理员 SID", "ipc.peerRole", GetLastError()));
    }
    BOOL member = FALSE;
    const BOOL checked = CheckTokenMembership(nullptr, administrators, &member);
    FreeSid(administrators);
    if (checked == FALSE)
    {
        return Result<bool>::failure(win32_error("IPC_UNAUTHORIZED", "无法检查客户端管理员身份",
                                                 "ipc.peerRole", GetLastError()));
    }
    return Result<bool>::success(member != FALSE);
}

} // namespace

Result<LocalIpcPeerInfo> inspect_local_named_pipe_peer(const std::uintptr_t native_handle) noexcept
{
    const HANDLE pipe = reinterpret_cast<HANDLE>(native_handle);
    auto client_name = client_computer_name(pipe);
    if (!client_name)
    {
        return Result<LocalIpcPeerInfo>::failure(client_name.error());
    }
    auto machine_name = local_computer_name();
    if (!machine_name)
    {
        return Result<LocalIpcPeerInfo>::failure(machine_name.error());
    }
    if (!equal_case_insensitive(client_name.value(), machine_name.value()) &&
        client_name.value() != L".")
    {
        return Result<LocalIpcPeerInfo>::success(
            {.actor_sid = {}, .local = false, .authenticated = false, .administrator = false});
    }

    if (ImpersonateNamedPipeClient(pipe) == FALSE)
    {
        return Result<LocalIpcPeerInfo>::failure(win32_error(
            "IPC_UNAUTHORIZED", "无法模拟命名管道客户端", "ipc.peerImpersonate", GetLastError()));
    }
    RevertGuard revert;

    HANDLE raw_token = nullptr;
    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &raw_token) == FALSE)
    {
        return Result<LocalIpcPeerInfo>::failure(win32_error(
            "IPC_UNAUTHORIZED", "无法打开客户端线程令牌", "ipc.peerToken", GetLastError()));
    }
    const std::unique_ptr<void, decltype(&CloseHandle)> token{raw_token, &CloseHandle};
    auto sid = current_token_sid(raw_token);
    if (!sid)
    {
        return Result<LocalIpcPeerInfo>::failure(sid.error());
    }
    auto administrator = current_token_is_administrator();
    if (!administrator)
    {
        return Result<LocalIpcPeerInfo>::failure(administrator.error());
    }

    if (RevertToSelf() == FALSE)
    {
        return Result<LocalIpcPeerInfo>::failure(win32_error(
            "IPC_UNAUTHORIZED", "无法恢复服务线程身份", "ipc.peerRevert", GetLastError()));
    }
    revert.release();
    const bool authenticated = sid.value() != "S-1-5-7";
    return Result<LocalIpcPeerInfo>::success({.actor_sid = std::move(sid).value(),
                                              .local = true,
                                              .authenticated = authenticated,
                                              .administrator = administrator.value()});
}

NamedInstanceGuard::NamedInstanceGuard(void* handle) noexcept : handle_(handle) {}

NamedInstanceGuard::~NamedInstanceGuard()
{
    if (handle_ != nullptr)
    {
        static_cast<void>(ReleaseMutex(handle_));
        CloseHandle(handle_);
    }
}

NamedInstanceGuard::NamedInstanceGuard(NamedInstanceGuard&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr))
{
}

NamedInstanceGuard& NamedInstanceGuard::operator=(NamedInstanceGuard&& other) noexcept
{
    if (this != &other)
    {
        if (handle_ != nullptr)
        {
            static_cast<void>(ReleaseMutex(handle_));
            CloseHandle(handle_);
        }
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

Result<std::unique_ptr<NamedInstanceGuard>> NamedInstanceGuard::acquire(
    const std::wstring& name) noexcept
{
    HANDLE handle = CreateMutexW(nullptr, TRUE, name.c_str());
    if (handle == nullptr)
    {
        return Result<std::unique_ptr<NamedInstanceGuard>>::failure(
            win32_error("SYS_SERVICE_START_FAILED", "无法创建 IPC 单实例保护", "ipc.instanceGuard",
                        GetLastError()));
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(handle);
        return Result<std::unique_ptr<NamedInstanceGuard>>::failure(
            make_error("IPC_BUSY", Severity::warning, "IPC 服务端实例已经运行", "ipc",
                       "ipc.instanceGuard", true));
    }
    return Result<std::unique_ptr<NamedInstanceGuard>>::success(
        std::make_unique<NamedInstanceGuard>(handle));
}

} // namespace paperbreak::platform
