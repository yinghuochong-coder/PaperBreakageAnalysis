#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace paperbreak
{

/// Dependency-injected RAII factory used by modules that own business threads.
/// The returned opaque owner must remain alive for the complete thread entry function.
using ThreadRegistrationFactory = std::function<std::shared_ptr<void>(std::string_view)>;

/// Optional diagnostics dependency. Call enabled before constructing per-item text.
struct DebugDiagnosticSink final
{
    std::function<bool()> enabled;
    std::function<void(std::string)> record;
};

} // namespace paperbreak
