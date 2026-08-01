#pragma once

#include "paperbreak/config/basic_config.hpp"
#include "paperbreak/platform/atomic_file.hpp"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace paperbreak::config
{

inline constexpr std::size_t default_config_history_limit = 5U;

enum class ConfigChangeSource
{
    startup_recovery,
    local_file,
    local_ipc,
    uplink,
};

struct ConfigChangeContext final
{
    ConfigChangeSource source{ConfigChangeSource::local_file};
    std::string actor;
    std::string correlation_id;
};

struct ConfigAuditRecord final
{
    ConfigChangeSource source{ConfigChangeSource::local_file};
    std::string actor;
    std::string correlation_id;
    std::uint64_t previous_revision{};
    std::uint64_t candidate_revision{};
    std::string timestamp;
    std::vector<std::string> changed_paths;
    struct Change final
    {
        std::string path;
        std::string previous_value;
        std::string candidate_value;
    };
    std::vector<Change> redacted_changes;
};

class IConfigAuditSink
{
  public:
    virtual ~IConfigAuditSink() = default;
    [[nodiscard]] virtual Result<void> record(const ConfigAuditRecord& record) = 0;
};

class IConfigApplier
{
  public:
    virtual ~IConfigApplier() = default;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual Result<void> prepare(const EdgeConfig& current,
                                               const EdgeConfig& candidate,
                                               const std::vector<std::string>& changed_paths) = 0;
    [[nodiscard]] virtual Result<void> apply_and_readback(const EdgeConfig& candidate) = 0;
    [[nodiscard]] virtual Result<void> commit(const EdgeConfig& candidate) = 0;
    [[nodiscard]] virtual Result<void> rollback(const EdgeConfig& previous) noexcept = 0;
};

struct ConfigSnapshot final
{
    std::shared_ptr<const EdgeConfig> stored;
    std::shared_ptr<const EdgeConfig> effective;
    std::uint64_t stored_config_revision{};
    std::uint64_t effective_config_revision{};
    std::vector<std::string> pending_restart_paths;
    bool recovered_from_history{};
};

class ConfigRepository final
{
  public:
    ConfigRepository(std::filesystem::path config_path, platform::IAtomicFileSystem& file_system,
                     IConfigAuditSink& audit_sink, std::vector<IConfigApplier*> appliers = {},
                     std::size_t history_limit = default_config_history_limit);

    [[nodiscard]] Result<ConfigSnapshot> load();
    [[nodiscard]] Result<ConfigSnapshot> update(std::string_view candidate_json,
                                                std::uint64_t expected_revision,
                                                const ConfigChangeContext& context);
    [[nodiscard]] Result<ConfigSnapshot> reload(std::uint64_t expected_revision,
                                                const ConfigChangeContext& context);
    [[nodiscard]] Result<ConfigSnapshot> rollback_to(std::uint64_t historical_revision,
                                                     std::uint64_t expected_revision,
                                                     const ConfigChangeContext& context);
    [[nodiscard]] Result<ConfigSnapshot> snapshot() const;
    void stop_accepting_changes() noexcept;

  private:
    [[nodiscard]] Result<ConfigSnapshot> update_locked(std::string candidate_json,
                                                       std::uint64_t expected_revision,
                                                       const ConfigChangeContext& context,
                                                       bool restore_disk_on_failure);
    [[nodiscard]] Result<EdgeConfig> parse_text(std::string_view text) const;
    [[nodiscard]] Result<void> persist_locked(const EdgeConfig& candidate);
    [[nodiscard]] Result<void> prune_history_locked();
    [[nodiscard]] std::filesystem::path history_directory() const;
    [[nodiscard]] std::filesystem::path history_path(std::uint64_t revision) const;
    [[nodiscard]] ConfigSnapshot make_snapshot_locked(bool recovered = false) const;

    std::filesystem::path config_path_;
    platform::IAtomicFileSystem& file_system_;
    IConfigAuditSink& audit_sink_;
    std::vector<IConfigApplier*> appliers_;
    std::size_t history_limit_;
    mutable std::mutex mutex_;
    std::shared_ptr<const EdgeConfig> stored_;
    std::shared_ptr<const EdgeConfig> effective_;
    std::uint64_t effective_revision_{};
    std::vector<std::string> pending_restart_paths_;
    bool accepting_changes_{true};
};

[[nodiscard]] std::string_view config_change_source_name(ConfigChangeSource source) noexcept;

} // namespace paperbreak::config
