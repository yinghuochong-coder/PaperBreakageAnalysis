#pragma once

#include "paperbreak/common/result.hpp"
#include "paperbreak/storage/nvme_index.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace paperbreak::storage
{

inline constexpr std::size_t nvme_default_recovery_maximum_files = 100'000U;
inline constexpr std::size_t nvme_default_recovery_summary_bytes = 64U * 1024U * 1024U;
inline constexpr std::chrono::milliseconds nvme_default_recovery_timeout{std::chrono::minutes{5}};

struct NvmeRecoveryLimits final
{
    std::size_t maximum_files{nvme_default_recovery_maximum_files};
    std::size_t maximum_summary_bytes{nvme_default_recovery_summary_bytes};
    std::chrono::steady_clock::time_point deadline;
};

struct NvmeRecoveryReport final
{
    std::vector<NvmeIndexedBlock> blocks;
    std::size_t scanned_files{};
    std::size_t accepted_blocks{};
    std::size_t repaired_blocks{};
    std::size_t quarantined_blocks{};
    std::uint64_t recovered_bytes{};
};

/// Scans and repairs v1 block files. It never writes the derived SQLite index.
class INvmeBlockRecovery
{
  public:
    virtual ~INvmeBlockRecovery() = default;
    [[nodiscard]] virtual Result<NvmeRecoveryReport> recover(const std::filesystem::path& root,
                                                             const NvmeRecoveryLimits& limits) = 0;
};

[[nodiscard]] std::shared_ptr<INvmeBlockRecovery> make_windows_nvme_block_recovery();

} // namespace paperbreak::storage
