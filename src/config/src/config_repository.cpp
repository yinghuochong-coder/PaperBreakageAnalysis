#include "paperbreak/config/config_repository.hpp"

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <sstream>

#include <nlohmann/json.hpp>

namespace paperbreak::config
{
namespace
{

Error repository_error(std::string code, Severity severity, std::string message,
                       std::string operation, bool retryable = false)
{
    return make_error(std::move(code), severity, std::move(message), "config", std::move(operation),
                      retryable);
}

bool same_content(EdgeConfig left, EdgeConfig right)
{
    right.config_revision = left.config_revision;
    right.modified_at = left.modified_at;
    return left == right;
}

void restore_restart_section(EdgeConfig& target, const EdgeConfig& effective,
                             const std::string_view path)
{
    if (path == "/system")
        target.system = effective.system;
    else if (path == "/cameras")
        target.cameras = effective.cameras;
    else if (path == "/acquisition")
        target.acquisition = effective.acquisition;
    else if (path == "/storage/roots")
    {
        target.storage.event_root = effective.storage.event_root;
        target.storage.cache_root = effective.storage.cache_root;
    }
    else if (path == "/uplink/transport")
    {
        target.uplink.server_url = effective.uplink.server_url;
        target.uplink.credential_reference = effective.uplink.credential_reference;
        target.uplink.certificate_reference = effective.uplink.certificate_reference;
    }
    else if (path == "/plantIo")
        target.plant_io = effective.plant_io;
    else if (path == "/logging/runtime")
    {
        target.logging.directory = effective.logging.directory;
        target.logging.queue_capacity = effective.logging.queue_capacity;
        target.logging.maximum_file_size_mib = effective.logging.maximum_file_size_mib;
        target.logging.maximum_files_per_day = effective.logging.maximum_files_per_day;
    }
}

std::optional<std::uint64_t> revision_from_history_path(const std::filesystem::path& path)
{
    const std::string stem = path.stem().string();
    std::uint64_t revision = 0;
    const auto result = std::from_chars(stem.data(), stem.data() + stem.size(), revision);
    if (result.ec != std::errc{} || result.ptr != stem.data() + stem.size())
    {
        return std::nullopt;
    }
    return revision;
}

void redact_references(nlohmann::json& value)
{
    if (value.is_object())
    {
        for (auto& [key, child] : value.items())
        {
            if (key == "credentialReference" || key == "certificateReference")
                child = child.is_string() && !child.get_ref<const std::string&>().empty()
                            ? "<reference>"
                            : "";
            else
                redact_references(child);
        }
    }
    else if (value.is_array())
    {
        for (auto& child : value)
            redact_references(child);
    }
}

std::string audit_value(const EdgeConfig& config, const std::string_view path)
{
    try
    {
        const auto document = nlohmann::json::parse(serialize_config(config));
        nlohmann::json value;
        if (path == "/cameras/parameters")
            value = document.at("cameras");
        else if (path == "/storage/roots")
            value = {{"eventRoot", document.at("storage").at("eventRoot")},
                     {"cacheRoot", document.at("storage").at("cacheRoot")}};
        else if (path == "/storage/watermarks")
            value = document.at("storage");
        else if (path == "/uplink/transport")
            value = {{"serverUrl", document.at("uplink").at("serverUrl")},
                     {"credentialReference", document.at("uplink").at("credentialReference")},
                     {"certificateReference", document.at("uplink").at("certificateReference")}};
        else if (path == "/uplink/runtime")
            value = {{"enabled", document.at("uplink").at("enabled")},
                     {"heartbeatSeconds", document.at("uplink").at("heartbeatSeconds")}};
        else if (path == "/logging/runtime")
            value = document.at("logging");
        else if (path == "/logging/live")
            value = {{"level", document.at("logging").at("level")},
                     {"retentionDays", document.at("logging").at("retentionDays")}};
        else
            value = document.at(nlohmann::json::json_pointer{std::string{path}});
        redact_references(value);
        auto text = value.dump();
        constexpr std::size_t maximum_audit_value_bytes = 4096U;
        if (text.size() > maximum_audit_value_bytes)
            text = text.substr(0U, maximum_audit_value_bytes) + "<truncated>";
        return text;
    }
    catch (...)
    {
        return "<unavailable>";
    }
}

std::vector<ConfigAuditRecord::Change> make_audit_changes(const EdgeConfig& previous,
                                                          const EdgeConfig& candidate,
                                                          const std::vector<std::string>& paths)
{
    std::vector<ConfigAuditRecord::Change> changes;
    changes.reserve(paths.size());
    for (const auto& path : paths)
        changes.push_back({path, audit_value(previous, path), audit_value(candidate, path)});
    return changes;
}

} // namespace

ConfigRepository::ConfigRepository(std::filesystem::path config_path,
                                   platform::IAtomicFileSystem& file_system,
                                   IConfigAuditSink& audit_sink,
                                   std::vector<IConfigApplier*> appliers,
                                   const std::size_t history_limit)
    : config_path_(std::move(config_path)), file_system_(file_system), audit_sink_(audit_sink),
      appliers_(std::move(appliers)), history_limit_(history_limit)
{
}

Result<EdgeConfig> ConfigRepository::parse_text(const std::string_view text) const
{
    return parse_config(text, config_path_.parent_path());
}

std::filesystem::path ConfigRepository::history_directory() const
{
    return config_path_.parent_path() / (config_path_.filename().wstring() + L".history");
}

std::filesystem::path ConfigRepository::history_path(const std::uint64_t revision) const
{
    std::ostringstream name;
    name << std::setw(20) << std::setfill('0') << revision << ".json";
    return history_directory() / name.str();
}

ConfigSnapshot ConfigRepository::make_snapshot_locked(const bool recovered) const
{
    return {.stored = stored_,
            .effective = effective_,
            .stored_config_revision = stored_ ? stored_->config_revision : 0U,
            .effective_config_revision = effective_revision_,
            .pending_restart_paths = pending_restart_paths_,
            .recovered_from_history = recovered};
}

Result<ConfigSnapshot> ConfigRepository::load()
{
    std::scoped_lock lock{mutex_};
    if (stored_)
    {
        return Result<ConfigSnapshot>::success(make_snapshot_locked());
    }
    auto current_text = file_system_.read_bounded(config_path_, config_max_bytes);
    if (current_text)
    {
        auto parsed = parse_text(current_text.value());
        if (parsed)
        {
            stored_ = std::make_shared<const EdgeConfig>(parsed.value());
            effective_ = stored_;
            effective_revision_ = stored_->config_revision;
            return Result<ConfigSnapshot>::success(make_snapshot_locked());
        }
    }

    auto history_files = file_system_.list_regular_files(history_directory());
    if (!history_files)
    {
        return Result<ConfigSnapshot>::failure(history_files.error());
    }
    std::vector<std::pair<std::uint64_t, std::filesystem::path>> candidates;
    for (const auto& path : history_files.value())
    {
        if (const auto revision = revision_from_history_path(path); revision.has_value())
        {
            candidates.emplace_back(revision.value(), path);
        }
    }
    std::ranges::sort(candidates, std::greater<>{},
                      &std::pair<std::uint64_t, std::filesystem::path>::first);
    for (const auto& [revision, path] : candidates)
    {
        auto history_text = file_system_.read_bounded(path, config_max_bytes);
        if (!history_text)
            continue;
        auto parsed = parse_text(history_text.value());
        if (!parsed || parsed.value().config_revision != revision)
            continue;
        ConfigAuditRecord record{
            .source = ConfigChangeSource::startup_recovery,
            .actor = "service",
            .correlation_id = "startup",
            .previous_revision = 0U,
            .candidate_revision = revision,
            .timestamp = current_utc_timestamp(),
            .changed_paths = {"/"},
            .redacted_changes = {{"/", "<invalid-main-config>", "<recovered-config>"}}};
        auto audit_result = audit_sink_.record(record);
        if (!audit_result)
            return Result<ConfigSnapshot>::failure(audit_result.error());
        auto restore_result =
            file_system_.replace_atomically(config_path_, serialize_config(parsed.value()));
        if (!restore_result)
            return Result<ConfigSnapshot>::failure(restore_result.error());
        stored_ = std::make_shared<const EdgeConfig>(parsed.value());
        effective_ = stored_;
        effective_revision_ = stored_->config_revision;
        return Result<ConfigSnapshot>::success(make_snapshot_locked(true));
    }

    Error error = current_text ? repository_error("SYS_CONFIG_INVALID", Severity::critical,
                                                  "主配置无效且没有可恢复的历史版本", "config.load")
                               : current_text.error();
    return Result<ConfigSnapshot>::failure(std::move(error));
}

Result<void> ConfigRepository::prune_history_locked()
{
    auto files = file_system_.list_regular_files(history_directory());
    if (!files)
        return Result<void>::failure(files.error());
    std::vector<std::pair<std::uint64_t, std::filesystem::path>> revisions;
    for (const auto& path : files.value())
    {
        if (const auto revision = revision_from_history_path(path); revision.has_value())
            revisions.emplace_back(revision.value(), path);
    }
    std::ranges::sort(revisions, {}, &std::pair<std::uint64_t, std::filesystem::path>::first);
    while (revisions.size() >= history_limit_ && !revisions.empty())
    {
        auto removed = file_system_.remove_file(revisions.front().second);
        if (!removed)
            return removed;
        revisions.erase(revisions.begin());
    }
    return Result<void>::success();
}

Result<void> ConfigRepository::persist_locked(const EdgeConfig& candidate)
{
    if (history_limit_ == 0U)
    {
        return Result<void>::failure(repository_error("SYS_CONFIG_PERSIST_FAILED",
                                                      Severity::critical, "配置历史上限不能为零",
                                                      "config.persist"));
    }
    auto directories = file_system_.create_directories(history_directory());
    if (!directories)
        return directories;
    auto prune = prune_history_locked();
    if (!prune)
        return prune;
    auto history_write = file_system_.replace_atomically(history_path(stored_->config_revision),
                                                         serialize_config(*stored_));
    if (!history_write)
        return history_write;
    return file_system_.replace_atomically(config_path_, serialize_config(candidate));
}

Result<ConfigSnapshot> ConfigRepository::update(std::string_view candidate_json,
                                                const std::uint64_t expected_revision,
                                                const ConfigChangeContext& context)
{
    std::scoped_lock lock{mutex_};
    return update_locked(std::string{candidate_json}, expected_revision, context, false);
}

Result<ConfigSnapshot> ConfigRepository::reload(const std::uint64_t expected_revision,
                                                const ConfigChangeContext& context)
{
    std::scoped_lock lock{mutex_};
    if (!stored_)
    {
        return Result<ConfigSnapshot>::failure(repository_error(
            "SYS_CONFIG_INVALID", Severity::error, "配置仓储尚未加载", "config.reload"));
    }
    const std::string previous = serialize_config(*stored_);
    auto candidate = file_system_.read_bounded(config_path_, config_max_bytes);
    if (!candidate)
        return Result<ConfigSnapshot>::failure(candidate.error());
    auto result = update_locked(std::move(candidate).value(), expected_revision, context, true);
    if (!result)
    {
        auto restored = file_system_.replace_atomically(config_path_, previous);
        if (!restored)
        {
            Error error = restored.error();
            error.details.push_back({"originalBusinessCode", result.error().business_code});
            return Result<ConfigSnapshot>::failure(std::move(error));
        }
    }
    return result;
}

Result<ConfigSnapshot> ConfigRepository::rollback_to(const std::uint64_t historical_revision,
                                                     const std::uint64_t expected_revision,
                                                     const ConfigChangeContext& context)
{
    auto historical =
        file_system_.read_bounded(history_path(historical_revision), config_max_bytes);
    if (!historical)
        return Result<ConfigSnapshot>::failure(historical.error());
    return update(historical.value(), expected_revision, context);
}

Result<ConfigSnapshot> ConfigRepository::update_locked(std::string candidate_json,
                                                       const std::uint64_t expected_revision,
                                                       const ConfigChangeContext& context,
                                                       const bool restore_disk_on_failure)
{
    static_cast<void>(restore_disk_on_failure);
    if (!accepting_changes_)
    {
        return Result<ConfigSnapshot>::failure(
            repository_error("SYS_SERVICE_STOPPING", Severity::warning,
                             "服务正在停止，拒绝配置修改", "config.update", true));
    }
    if (!stored_ || !effective_)
    {
        return Result<ConfigSnapshot>::failure(repository_error(
            "SYS_CONFIG_INVALID", Severity::error, "配置仓储尚未加载", "config.update"));
    }
    if (expected_revision != stored_->config_revision)
    {
        Error error = repository_error("SYS_CONFIG_VERSION_CONFLICT", Severity::warning,
                                       "配置修订与当前版本冲突", "config.update");
        error.details.push_back({"expectedConfigRevision", std::to_string(expected_revision)});
        error.details.push_back(
            {"currentConfigRevision", std::to_string(stored_->config_revision)});
        return Result<ConfigSnapshot>::failure(std::move(error));
    }

    auto parsed = parse_text(candidate_json);
    if (!parsed)
        return Result<ConfigSnapshot>::failure(parsed.error());
    if (parsed.value().config_revision != expected_revision)
    {
        Error error =
            repository_error("SYS_CONFIG_VERSION_CONFLICT", Severity::warning,
                             "候选文件修订必须等于 expectedConfigRevision", "config.update");
        error.details.push_back(
            {"candidateConfigRevision", std::to_string(parsed.value().config_revision)});
        return Result<ConfigSnapshot>::failure(std::move(error));
    }
    if (same_content(*stored_, parsed.value()))
    {
        return Result<ConfigSnapshot>::success(make_snapshot_locked());
    }

    EdgeConfig candidate = std::move(parsed).value();
    candidate.config_revision = stored_->config_revision + 1U;
    candidate.modified_at = current_utc_timestamp();
    const auto changed = changed_config_paths(*stored_, candidate);

    std::vector<std::string> pending;
    for (const auto& path : changed_config_paths(*effective_, candidate))
    {
        if (is_restart_required_path(path))
            pending.push_back(path);
    }
    EdgeConfig effective_candidate = candidate;
    for (const auto& path : pending)
        restore_restart_section(effective_candidate, *effective_, path);

    ConfigAuditRecord audit{.source = context.source,
                            .actor = context.actor,
                            .correlation_id = context.correlation_id,
                            .previous_revision = stored_->config_revision,
                            .candidate_revision = candidate.config_revision,
                            .timestamp = candidate.modified_at,
                            .changed_paths = changed,
                            .redacted_changes = make_audit_changes(*stored_, candidate, changed)};
    auto audit_result = audit_sink_.record(audit);
    if (!audit_result)
        return Result<ConfigSnapshot>::failure(audit_result.error());

    for (auto* applier : appliers_)
    {
        auto prepared = applier->prepare(*effective_, effective_candidate, changed);
        if (!prepared)
        {
            Error error =
                repository_error("SYS_CONFIG_APPLY_FAILED", Severity::error, "配置组件准备失败",
                                 "config.prepare", prepared.error().retryable);
            error.details.push_back({"component", std::string{applier->name()}});
            error.details.push_back({"causeBusinessCode", prepared.error().business_code});
            return Result<ConfigSnapshot>::failure(std::move(error));
        }
    }

    std::size_t applied_count = 0U;
    for (; applied_count < appliers_.size(); ++applied_count)
    {
        auto applied = appliers_[applied_count]->apply_and_readback(effective_candidate);
        if (!applied)
        {
            for (std::size_t rollback = applied_count + 1U; rollback > 0U; --rollback)
                static_cast<void>(appliers_[rollback - 1U]->rollback(*effective_));
            Error error = repository_error("SYS_CONFIG_APPLY_FAILED", Severity::error,
                                           "配置组件应用或回读失败", "config.apply",
                                           applied.error().retryable);
            error.details.push_back({"component", std::string{appliers_[applied_count]->name()}});
            error.details.push_back({"causeBusinessCode", applied.error().business_code});
            return Result<ConfigSnapshot>::failure(std::move(error));
        }
    }

    auto persisted = persist_locked(candidate);
    if (!persisted)
    {
        for (std::size_t rollback = applied_count; rollback > 0U; --rollback)
            static_cast<void>(appliers_[rollback - 1U]->rollback(*effective_));
        return Result<ConfigSnapshot>::failure(persisted.error());
    }

    std::size_t committed_count = 0U;
    for (; committed_count < appliers_.size(); ++committed_count)
    {
        auto committed = appliers_[committed_count]->commit(effective_candidate);
        if (!committed)
        {
            auto restore =
                file_system_.replace_atomically(config_path_, serialize_config(*stored_));
            for (std::size_t rollback = applied_count; rollback > 0U; --rollback)
                static_cast<void>(appliers_[rollback - 1U]->rollback(*effective_));
            if (!restore)
            {
                Error error = restore.error();
                error.details.push_back(
                    {"commitComponent", std::string{appliers_[committed_count]->name()}});
                return Result<ConfigSnapshot>::failure(std::move(error));
            }
            Error error =
                repository_error("SYS_CONFIG_APPLY_FAILED", Severity::error, "配置组件提交失败",
                                 "config.commit", committed.error().retryable);
            error.details.push_back({"component", std::string{appliers_[committed_count]->name()}});
            error.details.push_back({"causeBusinessCode", committed.error().business_code});
            return Result<ConfigSnapshot>::failure(std::move(error));
        }
    }

    stored_ = std::make_shared<const EdgeConfig>(std::move(candidate));
    effective_ = std::make_shared<const EdgeConfig>(std::move(effective_candidate));
    pending_restart_paths_ = std::move(pending);
    if (pending_restart_paths_.empty())
        effective_revision_ = stored_->config_revision;
    return Result<ConfigSnapshot>::success(make_snapshot_locked());
}

Result<ConfigSnapshot> ConfigRepository::snapshot() const
{
    std::scoped_lock lock{mutex_};
    if (!stored_)
    {
        return Result<ConfigSnapshot>::failure(repository_error(
            "SYS_CONFIG_INVALID", Severity::error, "配置仓储尚未加载", "config.snapshot"));
    }
    return Result<ConfigSnapshot>::success(make_snapshot_locked());
}

void ConfigRepository::stop_accepting_changes() noexcept
{
    std::scoped_lock lock{mutex_};
    accepting_changes_ = false;
}

std::string_view config_change_source_name(const ConfigChangeSource source) noexcept
{
    switch (source)
    {
    case ConfigChangeSource::startup_recovery:
        return "startup-recovery";
    case ConfigChangeSource::local_file:
        return "local-file";
    case ConfigChangeSource::local_ipc:
        return "local-ipc";
    case ConfigChangeSource::uplink:
        return "uplink";
    }
    return "unknown";
}

} // namespace paperbreak::config
