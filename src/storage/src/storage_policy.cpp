#include "paperbreak/storage/storage_policy.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>

namespace paperbreak::storage
{
namespace
{
Error policy_error(const std::string_view code, const Severity severity,
                   const std::string_view message, const std::string_view operation,
                   const std::filesystem::path& path = {}, const bool retryable = false)
{
    auto error = make_error(std::string{code}, severity, std::string{message}, "storage",
                            std::string{operation}, retryable);
    if (!path.empty())
        error.details.push_back({.key = "path", .value = path.string()});
    return error;
}

Error file_error(const std::string_view message, const std::string_view operation,
                 const std::filesystem::path& path, const std::error_code& native)
{
    auto error =
        policy_error("EVENT_DELETE_FAILED", Severity::error, message, operation, path, true);
    error.native_domain = "win32";
    error.native_code = std::to_string(native.value());
    error.details.push_back({.key = "nativeMessage", .value = native.message()});
    return error;
}

bool inside_or_equal(const std::filesystem::path& candidate, const std::filesystem::path& parent)
{
    const auto relative = candidate.lexically_relative(parent);
    if (relative.empty())
        return candidate == parent;
    const auto first = *relative.begin();
    return first != "..";
}

bool valid_deletion_paths(const EventRetentionRecord& event)
{
    std::vector<std::filesystem::path> event_components;
    for (const auto& component : event.relative_directory)
        event_components.push_back(component);
    std::vector<std::filesystem::path> deletion_components;
    for (const auto& component : event.deletion_relative_path)
        deletion_components.push_back(component);
    const auto safe = [](const auto& components) {
        return std::all_of(components.begin(), components.end(), [](const auto& component) {
            const auto value = component.generic_string();
            return !value.empty() && value != "." && value != "..";
        });
    };
    return !event.relative_directory.is_absolute() && event_components.size() == 4U &&
           safe(event_components) && !event_components.front().generic_string().starts_with('.') &&
           event_components.back().generic_string() == event.event_id &&
           !event.deletion_relative_path.is_absolute() && deletion_components.size() == 2U &&
           safe(deletion_components) && deletion_components.front() == ".deletions" &&
           deletion_components.back().generic_string() == event.event_id + ".deleting";
}

class StandardStoragePolicyFileSystem final : public IStoragePolicyFileSystem
{
  public:
    Result<StorageSpace> space(const std::filesystem::path& path) override
    {
        std::error_code error;
        const auto info = std::filesystem::space(path, error);
        if (error)
            return Result<StorageSpace>::failure(
                file_error("无法采样事件卷容量", "storage.space", path, error));
        return Result<StorageSpace>::success(
            {.capacity_bytes = info.capacity, .available_bytes = info.available});
    }

    Result<bool> is_directory(const std::filesystem::path& path) override
    {
        std::error_code error;
        const auto status = std::filesystem::symlink_status(path, error);
        if (error == std::errc::no_such_file_or_directory)
            return Result<bool>::success(false);
        if (error)
            return Result<bool>::failure(
                file_error("无法检查删除目录", "storage.delete.inspect", path, error));
        if (!std::filesystem::exists(status))
            return Result<bool>::success(false);
        if (std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status))
            return Result<bool>::failure(policy_error("EVENT_DELETE_FAILED", Severity::error,
                                                      "删除目标不是普通目录",
                                                      "storage.delete.inspect", path));
        return Result<bool>::success(true);
    }

    Result<void> create_directories(const std::filesystem::path& path) override
    {
        std::error_code error;
        std::filesystem::create_directories(path, error);
        if (error)
            return Result<void>::failure(
                file_error("无法创建删除暂存目录", "storage.delete.directory", path, error));
        return Result<void>::success();
    }

    Result<void> move_directory_atomically(const std::filesystem::path& source,
                                           const std::filesystem::path& destination) override
    {
        std::error_code error;
        std::filesystem::rename(source, destination, error);
        if (error)
            return Result<void>::failure(
                file_error("无法原子移动待删除事件", "storage.delete.move", source, error));
        return Result<void>::success();
    }

    Result<void> remove_tree(const std::filesystem::path& path) override
    {
        std::error_code error;
        static_cast<void>(std::filesystem::remove_all(path, error));
        if (error)
            return Result<void>::failure(
                file_error("无法删除暂存内容", "storage.delete.remove", path, error));
        return Result<void>::success();
    }

    Result<std::vector<std::filesystem::path>> expired_temporary_entries(
        const std::filesystem::path& root, const std::chrono::system_clock::time_point cutoff,
        const std::size_t maximum_entries) override
    {
        std::error_code error;
        if (!std::filesystem::exists(root, error))
            return error ? Result<std::vector<std::filesystem::path>>::failure(file_error(
                               "无法检查临时目录", "storage.temporary.inspect", root, error))
                         : Result<std::vector<std::filesystem::path>>::success({});
        std::vector<std::pair<std::chrono::system_clock::time_point, std::filesystem::path>> found;
        std::size_t scanned = 0U;
        for (std::filesystem::directory_iterator iterator{root, error}, end;
             !error && iterator != end && scanned < maximum_entries; iterator.increment(error))
        {
            ++scanned;
            const auto status = iterator->symlink_status(error);
            if (error)
                break;
            if (std::filesystem::is_symlink(status))
                continue;
            const auto file_time = iterator->last_write_time(error);
            if (error)
                break;
            const auto system_time =
                std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    file_time - std::filesystem::file_time_type::clock::now() +
                    std::chrono::system_clock::now());
            if (system_time <= cutoff)
                found.emplace_back(system_time, iterator->path());
        }
        if (error)
            return Result<std::vector<std::filesystem::path>>::failure(
                file_error("临时目录扫描失败", "storage.temporary.scan", root, error));
        std::sort(found.begin(), found.end(),
                  [](const auto& left, const auto& right) { return left.first < right.first; });
        std::vector<std::filesystem::path> paths;
        paths.reserve(found.size());
        for (auto& item : found)
            paths.push_back(std::move(item.second));
        return Result<std::vector<std::filesystem::path>>::success(std::move(paths));
    }
};

std::int64_t epoch_milliseconds(const std::chrono::system_clock::time_point value)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count();
}

} // namespace

std::string_view to_string(const StorageWatermark watermark) noexcept
{
    switch (watermark)
    {
    case StorageWatermark::normal:
        return "Normal";
    case StorageWatermark::warning:
        return "Warning";
    case StorageWatermark::critical:
        return "Critical";
    case StorageWatermark::stop_save:
        return "StopSave";
    }
    return "Unknown";
}

std::shared_ptr<IStoragePolicyFileSystem> make_storage_policy_file_system()
{
    return std::make_shared<StandardStoragePolicyFileSystem>();
}

struct StoragePolicyManager::Impl final
{
    StoragePolicyOptions options;
    EventMetadataDatabase* database{};
    std::shared_ptr<IStoragePolicyFileSystem> file_system;
    mutable std::mutex mutex;
    StoragePolicySnapshot snapshot;

    Result<void> refresh_snapshot()
    {
        auto space = file_system->space(options.event_root);
        if (!space)
            return Result<void>::failure(std::move(space).error());
        auto bytes = database->retained_event_bytes();
        if (!bytes)
            return Result<void>::failure(std::move(bytes).error());
        auto refreshed = snapshot;
        refreshed.capacity_bytes = space.value().capacity_bytes;
        refreshed.available_bytes = space.value().available_bytes;
        refreshed.retained_event_bytes = bytes.value();
        refreshed.watermark =
            StoragePolicyManager::classify(refreshed.available_bytes, options.watermarks);
        refreshed.ordinary_rolling_writes_allowed =
            refreshed.watermark == StorageWatermark::normal ||
            refreshed.watermark == StorageWatermark::warning;
        refreshed.large_writes_allowed = refreshed.watermark != StorageWatermark::stop_save;
        snapshot = refreshed;
        return Result<void>::success();
    }

    Result<void> delete_event(const EventRetentionRecord& event, const std::int64_t now_ms)
    {
        if (!valid_deletion_paths(event))
            return Result<void>::failure(policy_error(
                "EVENT_DELETE_FAILED", Severity::critical,
                "数据库中的事件或删除暂存路径不安全，拒绝文件操作", "storage.delete.validate"));
        const auto original = options.event_root / event.relative_directory;
        const auto staging = options.event_root / event.deletion_relative_path;
        auto original_exists = file_system->is_directory(original);
        if (!original_exists)
            return Result<void>::failure(std::move(original_exists).error());
        auto staging_exists = file_system->is_directory(staging);
        if (!staging_exists)
            return Result<void>::failure(std::move(staging_exists).error());
        if (original_exists.value() && staging_exists.value())
            return Result<void>::failure(
                policy_error("EVENT_DELETE_FAILED", Severity::error,
                             "正式目录与删除暂存目录同时存在，拒绝猜测删除",
                             "storage.delete.conflict", original));
        if (original_exists.value())
        {
            auto created = file_system->create_directories(staging.parent_path());
            if (!created)
                return created;
            auto moved = file_system->move_directory_atomically(original, staging);
            if (!moved)
                return moved;
            staging_exists = Result<bool>::success(true);
        }
        if (staging_exists.value())
        {
            auto removed = file_system->remove_tree(staging);
            if (!removed)
                return removed;
        }
        return database->complete_deletion(event.event_id, now_ms);
    }
};

Result<std::unique_ptr<StoragePolicyManager>> StoragePolicyManager::create(
    StoragePolicyOptions options, EventMetadataDatabase& database,
    std::shared_ptr<IStoragePolicyFileSystem> file_system)
{
    const auto& watermarks = options.watermarks;
    if (options.event_root.empty() || !file_system ||
        watermarks.warning_available_bytes <= watermarks.critical_available_bytes ||
        watermarks.critical_available_bytes <= watermarks.stop_save_available_bytes ||
        (options.retention_age && options.retention_age->count() <= 0) ||
        (options.maximum_event_bytes && *options.maximum_event_bytes == 0U) ||
        options.temporary_maximum_age.count() <= 0 || options.maximum_deletions_per_run == 0U ||
        options.maximum_deletions_per_run > database_maximum_page_size ||
        options.temporary_roots.size() > 16U || options.maximum_temporary_entries_per_run == 0U ||
        options.maximum_temporary_entries_per_run > 10000U)
        return Result<std::unique_ptr<StoragePolicyManager>>::failure(
            policy_error("SYS_CONFIG_INVALID", Severity::error, "存储水位或保留策略配置无效",
                         "storage.policy.create"));

    std::error_code error;
    options.event_root = std::filesystem::absolute(options.event_root, error).lexically_normal();
    if (error)
        return Result<std::unique_ptr<StoragePolicyManager>>::failure(policy_error(
            "SYS_CONFIG_INVALID", Severity::error, "事件根目录无效", "storage.policy.create"));
    for (auto& root : options.temporary_roots)
    {
        root = std::filesystem::absolute(root, error).lexically_normal();
        if (error || inside_or_equal(root, options.event_root))
            return Result<std::unique_ptr<StoragePolicyManager>>::failure(policy_error(
                "SYS_CONFIG_INVALID", Severity::error, "临时清理目录不得位于事件事实源目录内",
                "storage.policy.create", root));
    }
    auto manager = std::make_unique<StoragePolicyManager>(ConstructionKey{}, std::move(options),
                                                          database, std::move(file_system));
    auto refreshed = manager->impl_->refresh_snapshot();
    if (!refreshed)
        return Result<std::unique_ptr<StoragePolicyManager>>::failure(std::move(refreshed).error());
    return Result<std::unique_ptr<StoragePolicyManager>>::success(std::move(manager));
}

StoragePolicyManager::StoragePolicyManager(ConstructionKey, StoragePolicyOptions options,
                                           EventMetadataDatabase& database,
                                           std::shared_ptr<IStoragePolicyFileSystem> file_system)
    : impl_(std::make_unique<Impl>())
{
    impl_->options = std::move(options);
    impl_->database = &database;
    impl_->file_system = std::move(file_system);
}

StoragePolicyManager::~StoragePolicyManager() = default;

StorageWatermark StoragePolicyManager::classify(
    const std::uint64_t available_bytes, const StorageWatermarkThresholds& thresholds) noexcept
{
    if (available_bytes <= thresholds.stop_save_available_bytes)
        return StorageWatermark::stop_save;
    if (available_bytes <= thresholds.critical_available_bytes)
        return StorageWatermark::critical;
    if (available_bytes <= thresholds.warning_available_bytes)
        return StorageWatermark::warning;
    return StorageWatermark::normal;
}

Result<StorageMaintenanceReport> StoragePolicyManager::run_maintenance(
    const std::chrono::system_clock::time_point now)
{
    const auto now_ms = epoch_milliseconds(now);
    if (now_ms < 0)
        return Result<StorageMaintenanceReport>::failure(
            policy_error("SYS_CONFIG_INVALID", Severity::error, "维护时间早于 Unix epoch",
                         "storage.maintenance.validate"));
    const std::scoped_lock lock{impl_->mutex};
    StorageMaintenanceReport report;
    ++impl_->snapshot.maintenance_runs;

    auto work = impl_->database->deletion_work(impl_->options.maximum_deletions_per_run);
    if (!work)
        return Result<StorageMaintenanceReport>::failure(std::move(work).error());
    std::size_t work_count = 0U;
    for (const auto& event : work.value())
    {
        auto resumed = event;
        if (resumed.deletion_state == "DeleteFailed")
        {
            auto claimed = impl_->database->begin_deletion(resumed.event_id,
                                                           resumed.deletion_relative_path, now_ms);
            if (!claimed)
                return Result<StorageMaintenanceReport>::failure(std::move(claimed).error());
            if (!claimed.value())
                continue;
            resumed.deletion_state = "DeletePending";
        }
        auto deleted = impl_->delete_event(resumed, now_ms);
        if (deleted)
        {
            ++report.deletion_work_recovered;
            ++report.events_deleted;
            ++impl_->snapshot.events_deleted;
        }
        else
        {
            const auto reason = deleted.error().business_code + ": " + deleted.error().message;
            auto recorded = impl_->database->fail_deletion(resumed.event_id, reason, now_ms);
            report.failures.push_back(
                {.event_id = resumed.event_id,
                 .path = impl_->options.event_root / resumed.deletion_relative_path,
                 .error = std::move(deleted).error()});
            if (!recorded)
                report.failures.push_back(
                    {.event_id = resumed.event_id, .error = std::move(recorded).error()});
            ++impl_->snapshot.event_delete_failures;
        }
        ++work_count;
    }

    auto refreshed = impl_->refresh_snapshot();
    if (!refreshed)
        return Result<StorageMaintenanceReport>::failure(std::move(refreshed).error());
    const auto cutoff =
        impl_->options.retention_age
            ? std::optional<std::int64_t>{epoch_milliseconds(now - *impl_->options.retention_age)}
            : std::nullopt;
    const auto capacity_pressure = [&] {
        return impl_->options.maximum_event_bytes &&
               impl_->snapshot.retained_event_bytes > *impl_->options.maximum_event_bytes;
    };
    const auto watermark_pressure = [&] {
        return impl_->snapshot.watermark != StorageWatermark::normal;
    };
    const auto remaining_slots = impl_->options.maximum_deletions_per_run - work_count;
    if (remaining_slots > 0U && (watermark_pressure() || capacity_pressure() || cutoff))
    {
        const auto query_cutoff =
            watermark_pressure() || capacity_pressure() ? std::nullopt : cutoff;
        auto candidates = impl_->database->retention_candidates(query_cutoff, remaining_slots);
        if (!candidates)
            return Result<StorageMaintenanceReport>::failure(std::move(candidates).error());
        for (auto event : candidates.value())
        {
            const bool age_expired = cutoff && event.candidate_time_utc_ms <= *cutoff;
            if (!watermark_pressure() && !capacity_pressure() && !age_expired)
                break;
            event.deletion_relative_path =
                std::filesystem::path{".deletions"} / (event.event_id + ".deleting");
            auto claimed = impl_->database->begin_deletion(event.event_id,
                                                           event.deletion_relative_path, now_ms);
            if (!claimed)
                return Result<StorageMaintenanceReport>::failure(std::move(claimed).error());
            if (!claimed.value())
                continue;
            auto deleted = impl_->delete_event(event, now_ms);
            if (deleted)
            {
                ++report.events_deleted;
                ++impl_->snapshot.events_deleted;
            }
            else
            {
                const auto reason = deleted.error().business_code + ": " + deleted.error().message;
                auto recorded = impl_->database->fail_deletion(event.event_id, reason, now_ms);
                report.failures.push_back(
                    {.event_id = event.event_id,
                     .path = impl_->options.event_root / event.deletion_relative_path,
                     .error = std::move(deleted).error()});
                if (!recorded)
                    report.failures.push_back(
                        {.event_id = event.event_id, .error = std::move(recorded).error()});
                ++impl_->snapshot.event_delete_failures;
            }
            refreshed = impl_->refresh_snapshot();
            if (!refreshed)
                return Result<StorageMaintenanceReport>::failure(std::move(refreshed).error());
        }
    }

    const auto temporary_cutoff = now - impl_->options.temporary_maximum_age;
    std::size_t temporary_budget = impl_->options.maximum_temporary_entries_per_run;
    for (const auto& root : impl_->options.temporary_roots)
    {
        if (temporary_budget == 0U)
            break;
        auto entries =
            impl_->file_system->expired_temporary_entries(root, temporary_cutoff, temporary_budget);
        if (!entries)
        {
            report.failures.push_back({.path = root, .error = std::move(entries).error()});
            ++impl_->snapshot.temporary_delete_failures;
            continue;
        }
        for (const auto& entry : entries.value())
        {
            auto removed = impl_->file_system->remove_tree(entry);
            if (!removed)
            {
                report.failures.push_back({.path = entry, .error = std::move(removed).error()});
                ++impl_->snapshot.temporary_delete_failures;
            }
            else
            {
                ++report.temporary_entries_deleted;
                ++impl_->snapshot.temporary_entries_deleted;
            }
            --temporary_budget;
            if (temporary_budget == 0U)
                break;
        }
    }
    refreshed = impl_->refresh_snapshot();
    if (!refreshed)
        return Result<StorageMaintenanceReport>::failure(std::move(refreshed).error());
    report.snapshot = impl_->snapshot;
    return Result<StorageMaintenanceReport>::success(std::move(report));
}

Result<void> StoragePolicyManager::admit_large_write() const
{
    const std::scoped_lock lock{impl_->mutex};
    if (impl_->snapshot.large_writes_allowed)
        return Result<void>::success();
    auto error = policy_error("STORAGE_LOW_SPACE", Severity::critical,
                              "数据盘已达到停止保存水位，拒绝新增大文件",
                              "storage.admission.large-write", impl_->options.event_root, true);
    error.details.push_back(
        {.key = "availableBytes", .value = std::to_string(impl_->snapshot.available_bytes)});
    error.details.push_back(
        {.key = "stopSaveBytes",
         .value = std::to_string(impl_->options.watermarks.stop_save_available_bytes)});
    return Result<void>::failure(std::move(error));
}

Result<void> StoragePolicyManager::set_retention_age(const std::chrono::days retention_age)
{
    if (retention_age.count() <= 0)
        return Result<void>::failure(policy_error("SYS_CONFIG_INVALID", Severity::error,
                                                  "事件保留天数必须大于零",
                                                  "storage.policy.retention.configure"));
    const std::scoped_lock lock{impl_->mutex};
    impl_->options.retention_age = retention_age;
    return Result<void>::success();
}

Result<void> StoragePolicyManager::reconfigure_limits(
    const StorageWatermarkThresholds watermarks,
    const std::optional<std::uint64_t> maximum_event_bytes)
{
    if (watermarks.warning_available_bytes <= watermarks.critical_available_bytes ||
        watermarks.critical_available_bytes <= watermarks.stop_save_available_bytes ||
        (maximum_event_bytes && *maximum_event_bytes == 0U))
        return Result<void>::failure(policy_error("SYS_CONFIG_INVALID", Severity::error,
                                                  "存储水位或事件容量上限配置无效",
                                                  "storage.policy.limits.configure"));

    const std::scoped_lock lock{impl_->mutex};
    const auto previous_watermarks = impl_->options.watermarks;
    const auto previous_maximum_event_bytes = impl_->options.maximum_event_bytes;
    impl_->options.watermarks = watermarks;
    impl_->options.maximum_event_bytes = maximum_event_bytes;
    auto refreshed = impl_->refresh_snapshot();
    if (!refreshed)
    {
        impl_->options.watermarks = previous_watermarks;
        impl_->options.maximum_event_bytes = previous_maximum_event_bytes;
        return refreshed;
    }
    return Result<void>::success();
}

StoragePolicySnapshot StoragePolicyManager::snapshot() const noexcept
{
    const std::scoped_lock lock{impl_->mutex};
    return impl_->snapshot;
}

} // namespace paperbreak::storage
