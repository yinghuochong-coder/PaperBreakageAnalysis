#include "paperbreak/storage/nvme_cache.hpp"

#include "paperbreak/common/camera_slots.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <intrin.h>

namespace paperbreak::storage
{
namespace
{

Error nvme_error(std::string code, const Severity severity, std::string message,
                 std::string operation, std::string reason = {}, const bool retryable = false)
{
    auto error = make_error(std::move(code), severity, std::move(message), "storage",
                            std::move(operation), retryable);
    if (!reason.empty())
        error.details.push_back({"reason", std::move(reason)});
    return error;
}

std::array<std::byte, 16U> make_block_id(const std::string_view camera_id,
                                         const std::uint64_t generation) noexcept
{
    static std::atomic_uint64_t sequence{1U};
    const auto counter = sequence.fetch_add(1U, std::memory_order_relaxed);
    const auto ticks =
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    std::uint64_t left = ticks ^ (counter * 0x9E3779B97F4A7C15ULL);
    std::uint64_t right = generation ^ (counter * 0xD6E8FEB86659FD93ULL);
    for (const unsigned char value : camera_id)
    {
        left = (left ^ value) * 1099511628211ULL;
        right = (right << 7U) ^ (right >> 3U) ^ value;
    }
    std::array<std::byte, 16U> result{};
    for (std::size_t index = 0U; index < 8U; ++index)
    {
        result[index] = static_cast<std::byte>((left >> (index * 8U)) & 0xFFU);
        result[index + 8U] = static_cast<std::byte>((right >> (index * 8U)) & 0xFFU);
    }
    result[6U] = static_cast<std::byte>((std::to_integer<unsigned>(result[6U]) & 0x0FU) | 0x40U);
    result[8U] = static_cast<std::byte>((std::to_integer<unsigned>(result[8U]) & 0x3FU) | 0x80U);
    return result;
}

std::uint64_t saturating_add(const std::uint64_t left, const std::uint64_t right) noexcept
{
    if (right > (std::numeric_limits<std::uint64_t>::max)() - left)
        return (std::numeric_limits<std::uint64_t>::max)();
    return left + right;
}

constexpr std::array<std::uint32_t, 256U> make_crc32c_table() noexcept
{
    std::array<std::uint32_t, 256U> table{};
    std::uint32_t index = 0U;
    for (auto& entry : table)
    {
        auto value = index++;
        for (unsigned bit = 0U; bit < 8U; ++bit)
            value = (value >> 1U) ^ (0x82F63B78U & (0U - (value & 1U)));
        entry = value;
    }
    return table;
}

bool sse42_available() noexcept
{
    std::array<int, 4U> registers{};
    __cpuid(registers.data(), 1);
    return (registers[2] & (1 << 20)) != 0;
}

} // namespace

std::uint32_t crc32c(const std::span<const std::byte> bytes, const std::uint32_t seed) noexcept
{
    std::uint32_t crc = ~seed;
    static const bool use_sse42 = sse42_available();
    if (use_sse42)
    {
        auto remaining = bytes.size();
        const auto* current = bytes.data();
#if defined(_M_X64)
        while (remaining >= sizeof(std::uint64_t))
        {
            std::uint64_t value{};
            std::memcpy(&value, current, sizeof(value));
            crc = static_cast<std::uint32_t>(_mm_crc32_u64(crc, value));
            current += sizeof(value);
            remaining -= sizeof(value);
        }
#endif
        while (remaining > 0U)
        {
            crc = _mm_crc32_u8(crc, std::to_integer<std::uint8_t>(*current));
            ++current;
            --remaining;
        }
        return ~crc;
    }
    static constexpr auto table = make_crc32c_table();
    for (const auto byte : bytes)
        crc = table[(crc ^ std::to_integer<std::uint8_t>(byte)) & 0xffU] ^ (crc >> 8U);
    return ~crc;
}

Result<std::uint64_t> maximum_nvme_block_bytes(const std::uint32_t index_capacity,
                                               const std::uint32_t maximum_frame_bytes) noexcept
{
    constexpr std::uint64_t alignment = nvme_page_bytes;
    if (index_capacity == 0U || maximum_frame_bytes == 0U)
    {
        return Result<std::uint64_t>::failure(nvme_error("SYS_CONFIG_INVALID", Severity::error,
                                                         "NVMe 块容量参数无效",
                                                         "storage.nvme.capacity", "zero-capacity"));
    }
    const auto align = [](const std::uint64_t value) -> std::optional<std::uint64_t> {
        constexpr std::uint64_t mask = nvme_page_bytes - 1U;
        if (value > (std::numeric_limits<std::uint64_t>::max)() - mask)
            return std::nullopt;
        return (value + mask) & ~mask;
    };
    const auto index_bytes = static_cast<std::uint64_t>(index_capacity) * nvme_index_entry_bytes;
    const auto data_bytes = static_cast<std::uint64_t>(index_capacity) * maximum_frame_bytes;
    const auto aligned_index = align(index_bytes);
    const auto aligned_data = align(data_bytes);
    if (!aligned_index || !aligned_data ||
        *aligned_index >
            (std::numeric_limits<std::uint64_t>::max)() - *aligned_data - (2U * alignment))
    {
        return Result<std::uint64_t>::failure(
            nvme_error("SYS_CONFIG_INVALID", Severity::error, "NVMe 块容量计算溢出",
                       "storage.nvme.capacity", "capacity-overflow"));
    }
    return Result<std::uint64_t>::success(2U * alignment + *aligned_index + *aligned_data);
}

std::string_view to_string(const NvmeCacheState state) noexcept
{
    switch (state)
    {
    case NvmeCacheState::stopped:
        return "stopped";
    case NvmeCacheState::running:
        return "running";
    case NvmeCacheState::watermark_blocked:
        return "watermark-blocked";
    case NvmeCacheState::memory_degraded:
        return "memory-degraded";
    case NvmeCacheState::stopping:
        return "stopping";
    }
    return "stopped";
}

struct NvmeRollingCacheImpl final
{
    struct Lane final
    {
        NvmeCameraLayout layout;
        std::optional<NvmeBlock> current;
        std::size_t queued_blocks{};
        std::uint64_t next_generation{1U};
        std::optional<std::uint64_t> last_sequence;
        std::optional<camera::MonotonicTime> last_monotonic;
    };

    NvmeRollingCacheOptions options;
    std::shared_ptr<INvmeBlockStore> store;
    std::shared_ptr<INvmeBlockIndex> index;
    mutable std::mutex mutex;
    mutable std::mutex index_operation_mutex;
    std::condition_variable condition;
    std::unordered_map<std::string, Lane> lanes;
    std::deque<NvmeBlock> queue;
    std::jthread worker;
    NvmeRollingCacheSnapshot metrics;
    bool started{};
    bool stop_requested{};
    bool completed{true};
    std::size_t maximum_index_blocks{};

    [[nodiscard]] bool ordinary_writes_allowed_locked() const noexcept
    {
        return metrics.state == NvmeCacheState::running &&
               (metrics.storage_watermark == StorageWatermark::normal ||
                metrics.storage_watermark == StorageWatermark::warning);
    }

    void notify_error(const Error& error) const noexcept
    {
        if (!options.error_observer)
            return;
        try
        {
            options.error_observer(error);
        }
        catch (...)
        {
        }
    }

    [[nodiscard]] NvmeBlock new_block(Lane& lane, const camera::FrameView& first)
    {
        NvmeBlock result{.block_id = make_block_id(lane.layout.camera_id, lane.next_generation),
                         .generation = lane.next_generation++,
                         .camera_id = lane.layout.camera_id,
                         .start_monotonic_time = first.received_monotonic_time(),
                         .start_wall_clock_time = first.received_wall_clock_time(),
                         .maximum_frame_bytes = lane.layout.maximum_frame_bytes,
                         .index_capacity = lane.layout.index_capacity};
        result.frames.reserve(lane.layout.index_capacity);
        return result;
    }

    [[nodiscard]] bool enqueue_current_locked(Lane& lane)
    {
        if (!lane.current || lane.current->frames.empty())
        {
            lane.current.reset();
            return true;
        }
        ++metrics.completed_blocks;
        if (lane.queued_blocks >= options.queue_capacity_per_camera)
        {
            ++metrics.rejected_blocks;
            lane.current.reset();
            return false;
        }
        queue.push_back(std::move(*lane.current));
        lane.current.reset();
        ++lane.queued_blocks;
        ++metrics.enqueued_blocks;
        metrics.queue_depth = queue.size();
        metrics.queue_high_watermark = std::max(metrics.queue_high_watermark, queue.size());
        return true;
    }

    void discard_open_blocks_locked() noexcept
    {
        for (auto& [unused, lane] : lanes)
        {
            static_cast<void>(unused);
            if (lane.current && !lane.current->frames.empty())
                ++metrics.rejected_blocks;
            lane.current.reset();
        }
    }

    void degrade(Error error)
    {
        {
            std::scoped_lock lock{mutex};
            metrics.state = NvmeCacheState::memory_degraded;
            metrics.accepting = false;
            metrics.last_error = error;
            ++metrics.write_failures;
            if (error.business_code == "NVME_WRITE_TIMEOUT")
                ++metrics.write_timeouts;
            for (auto& block : queue)
            {
                static_cast<void>(block);
                ++metrics.rejected_blocks;
            }
            queue.clear();
            for (auto& [unused, lane] : lanes)
            {
                static_cast<void>(unused);
                lane.queued_blocks = 0U;
            }
            metrics.queue_depth = 0U;
            discard_open_blocks_locked();
            stop_requested = true;
        }
        notify_error(error);
        condition.notify_all();
    }

    [[nodiscard]] Result<void> reclaim_for(const std::uint64_t required)
    {
        for (;;)
        {
            {
                std::scoped_lock lock{mutex};
                if (metrics.current_cache_bytes <= options.maximum_cache_bytes - required)
                    return Result<void>::success();
            }
            std::scoped_lock index_lock{index_operation_mutex};
            auto oldest = index->oldest_reclaimable();
            if (!oldest)
                return Result<void>::failure(std::move(oldest).error());
            if (!oldest.value())
            {
                return Result<void>::failure(nvme_error(
                    "NVME_CACHE_PROTECTED", Severity::error, "NVMe 固定容量中的块均受事件租约保护",
                    "storage.nvme.reclaim", "no-unprotected-block", true));
            }
            auto removed = store->remove_committed(oldest.value()->path);
            if (!removed)
                return removed;
            auto erased = index->erase_block(oldest.value()->block_id);
            if (!erased)
                return erased;
            auto index_snapshot = index->snapshot();
            {
                std::scoped_lock lock{mutex};
                metrics.current_cache_bytes =
                    oldest.value()->physical_bytes > metrics.current_cache_bytes
                        ? 0U
                        : metrics.current_cache_bytes - oldest.value()->physical_bytes;
                ++metrics.blocks_reclaimed;
                metrics.bytes_reclaimed =
                    saturating_add(metrics.bytes_reclaimed, oldest.value()->physical_bytes);
                apply_index_snapshot_locked(index_snapshot);
            }
        }
    }

    void apply_index_snapshot_locked(const Result<NvmeBlockIndexSnapshot>& current)
    {
        if (!current)
        {
            metrics.last_error = current.error();
            return;
        }
        metrics.indexed_blocks = current.value().block_count;
        metrics.active_event_leases = current.value().active_leases;
        metrics.protected_blocks = current.value().protected_blocks;
        metrics.protected_bytes = current.value().protected_bytes;
    }

    void run(const std::stop_token token) noexcept
    {
        const auto thread_registration =
            options.register_thread ? options.register_thread("nvme-writer") : nullptr;
        for (;;)
        {
            NvmeBlock block;
            {
                std::unique_lock lock{mutex};
                condition.wait(lock, [&] {
                    return token.stop_requested() || !queue.empty() || stop_requested;
                });
                if (token.stop_requested() || (stop_requested && queue.empty()))
                    break;
                block = std::move(queue.front());
                queue.pop_front();
                auto lane = lanes.find(block.camera_id);
                if (lane != lanes.end() && lane->second.queued_blocks > 0U)
                    --lane->second.queued_blocks;
                metrics.queue_depth = queue.size();
            }

            const auto physical =
                maximum_nvme_block_bytes(block.index_capacity, block.maximum_frame_bytes);
            if (!physical)
            {
                degrade(physical.error());
                break;
            }
            auto reclaimed = reclaim_for(physical.value());
            if (!reclaimed)
            {
                degrade(reclaimed.error());
                break;
            }
            const auto deadline = std::chrono::steady_clock::now() + options.io_timeout;
            const auto write_started = std::chrono::steady_clock::now();
            auto written = store->write_block(
                {.root = options.root,
                 .block = &block,
                 .write_limit_bytes_per_second = options.write_limit_bytes_per_second,
                 .deadline = deadline},
                token);
            if (options.diagnostics.enabled && options.diagnostics.enabled() &&
                options.diagnostics.record)
                options.diagnostics.record(
                    "operation=nvme.write cameraId=" + block.camera_id +
                    " frameCount=" + std::to_string(block.frames.size()) +
                    " bytes=" + std::to_string(physical.value()) +
                    " result=" + (written ? "success" : "failure") +
                    " businessCode=" + (written ? "OK" : written.error().business_code));
            if (!written)
            {
                degrade(written.error());
                break;
            }
            std::uint64_t block_sequence_gaps{};
            for (std::size_t frame_index = 1U; frame_index < block.frames.size(); ++frame_index)
            {
                const auto previous = block.frames[frame_index - 1U].sequence_number();
                const auto current = block.frames[frame_index].sequence_number();
                if (current > previous + 1U)
                    block_sequence_gaps =
                        saturating_add(block_sequence_gaps, current - previous - 1U);
            }
            NvmeIndexedBlock indexed{
                .block_id = block.block_id,
                .camera_id = block.camera_id,
                .generation = block.generation,
                .path = written.value().path,
                .physical_bytes = written.value().physical_bytes,
                .start_monotonic_time = block.start_monotonic_time,
                .end_monotonic_time = block.frames.back().received_monotonic_time(),
                .start_wall_clock_time = block.start_wall_clock_time,
                .end_wall_clock_time = block.frames.back().received_wall_clock_time(),
                .start_sequence_number = block.frames.front().sequence_number(),
                .end_sequence_number = block.frames.back().sequence_number(),
                .frame_count = static_cast<std::uint32_t>(block.frames.size()),
                .sequence_gaps = block_sequence_gaps,
                .header_crc32c = written.value().header_crc32c,
                .index_crc32c = written.value().index_crc32c,
                .data_crc32c = written.value().data_crc32c,
                .footer_crc32c = written.value().footer_crc32c,
                .commit_verified = written.value().commit_verified};
            {
                std::scoped_lock index_lock{index_operation_mutex};
                auto registered = index->register_block(std::move(indexed));
                if (!registered)
                {
                    {
                        std::scoped_lock lock{mutex};
                        metrics.current_cache_bytes = saturating_add(
                            metrics.current_cache_bytes, written.value().physical_bytes);
                    }
                    degrade(registered.error());
                    break;
                }
            }
            auto index_snapshot = [&] {
                std::scoped_lock index_lock{index_operation_mutex};
                return index->snapshot();
            }();
            {
                std::scoped_lock lock{mutex};
                const auto elapsed =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - write_started);
                ++metrics.committed_blocks;
                metrics.bytes_committed =
                    saturating_add(metrics.bytes_committed, written.value().physical_bytes);
                metrics.current_cache_bytes =
                    saturating_add(metrics.current_cache_bytes, written.value().physical_bytes);
                metrics.write_bytes_per_second =
                    elapsed.count() > 0.0
                        ? static_cast<double>(written.value().physical_bytes) / elapsed.count()
                        : 0.0;
                apply_index_snapshot_locked(index_snapshot);
            }
        }
        {
            std::scoped_lock lock{mutex};
            completed = true;
            if (metrics.state != NvmeCacheState::memory_degraded)
                metrics.state = NvmeCacheState::stopped;
            metrics.accepting = false;
        }
        condition.notify_all();
    }
};

Result<std::shared_ptr<NvmeRollingCache>> NvmeRollingCache::create(
    NvmeRollingCacheOptions options, std::shared_ptr<INvmeBlockStore> store,
    std::shared_ptr<INvmeBlockIndex> index)
{
    if (!store || !index || options.root.empty() || options.maximum_cache_bytes == 0U ||
        options.write_limit_bytes_per_second == 0U ||
        options.io_timeout <= std::chrono::milliseconds::zero() ||
        options.queue_capacity_per_camera != nvme_default_queue_capacity_per_camera ||
        options.cameras.empty() || options.cameras.size() > camera_slot_count)
    {
        return Result<std::shared_ptr<NvmeRollingCache>>::failure(
            nvme_error("SYS_CONFIG_INVALID", Severity::error, "NVMe 滚动缓存配置无效",
                       "storage.nvme.create", "invalid-options"));
    }
    std::unordered_set<std::string> camera_ids;
    std::uint64_t required_rate{};
    auto impl = std::make_unique<NvmeRollingCacheImpl>();
    impl->options = std::move(options);
    impl->store = std::move(store);
    impl->index = std::move(index);
    std::uint64_t smallest_block = (std::numeric_limits<std::uint64_t>::max)();
    for (const auto& layout : impl->options.cameras)
    {
        const auto maximum =
            maximum_nvme_block_bytes(layout.index_capacity, layout.maximum_frame_bytes);
        if (!is_canonical_camera_id(layout.camera_id) || !maximum ||
            maximum.value() > impl->options.maximum_cache_bytes ||
            layout.required_input_bytes_per_second == 0U ||
            !camera_ids.insert(layout.camera_id).second)
        {
            return Result<std::shared_ptr<NvmeRollingCache>>::failure(
                nvme_error("SYS_CONFIG_INVALID", Severity::error, "NVMe 相机块布局无效",
                           "storage.nvme.create", "invalid-camera-layout"));
        }
        required_rate = saturating_add(required_rate, layout.required_input_bytes_per_second);
        smallest_block = std::min(smallest_block, maximum.value());
        impl->lanes.emplace(layout.camera_id, NvmeRollingCacheImpl::Lane{.layout = layout});
    }
    if (required_rate > impl->options.write_limit_bytes_per_second)
    {
        auto error =
            nvme_error("SYS_CONFIG_INVALID", Severity::error, "NVMe 写入限速低于完整原始输入需求",
                       "storage.nvme.create", "write-limit-below-input");
        error.details.push_back({"requiredBytesPerSecond", std::to_string(required_rate)});
        error.details.push_back({"writeLimitBytesPerSecond",
                                 std::to_string(impl->options.write_limit_bytes_per_second)});
        return Result<std::shared_ptr<NvmeRollingCache>>::failure(std::move(error));
    }
    impl->metrics.camera_count = impl->options.cameras.size();
    impl->metrics.queue_capacity =
        impl->options.cameras.size() * impl->options.queue_capacity_per_camera;
    impl->maximum_index_blocks =
        static_cast<std::size_t>(impl->options.maximum_cache_bytes / smallest_block);
    return Result<std::shared_ptr<NvmeRollingCache>>::success(
        std::make_shared<NvmeRollingCache>(ConstructionKey{}, std::move(impl)));
}

NvmeRollingCache::NvmeRollingCache(ConstructionKey, std::unique_ptr<NvmeRollingCacheImpl> impl)
    : impl_(std::move(impl))
{
}

NvmeRollingCache::~NvmeRollingCache()
{
    request_stop();
    static_cast<void>(join(std::chrono::steady_clock::now() + std::chrono::seconds{30}));
}

Result<void> NvmeRollingCache::start()
{
    {
        std::scoped_lock lock{impl_->mutex};
        if (impl_->started)
            return Result<void>::failure(nvme_error("SYS_INVALID_STATE", Severity::error,
                                                    "NVMe 写入器不能重复启动", "storage.nvme.start",
                                                    "already-started"));
        impl_->started = true;
        impl_->completed = false;
    }
    const auto sessions_root = impl_->options.root / "sessions";
    std::error_code directory_error;
    std::filesystem::create_directories(sessions_root, directory_error);
    std::filesystem::path session_root;
    static std::atomic_uint64_t session_sequence{1U};
    for (std::size_t attempt = 0U; !directory_error && attempt < 64U; ++attempt)
    {
        const auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
        const auto sequence = session_sequence.fetch_add(1U, std::memory_order_relaxed);
        const auto candidate =
            sessions_root / ("session-" + std::to_string(ticks) + "-" + std::to_string(sequence));
        if (std::filesystem::create_directory(candidate, directory_error))
        {
            session_root = candidate;
            break;
        }
        if (directory_error == std::errc::file_exists)
            directory_error.clear();
    }
    if (directory_error || session_root.empty())
    {
        impl_->degrade(nvme_error("NVME_CACHE_UNAVAILABLE", Severity::error,
                                  "无法唯一创建 NVMe 当前会话目录", "storage.nvme.session",
                                  "create-session-failed", true));
        std::scoped_lock lock{impl_->mutex};
        impl_->completed = true;
        return Result<void>::success();
    }
    impl_->options.root = session_root;
    {
        std::scoped_lock lock{impl_->mutex};
        impl_->metrics.active_session_root = session_root;
    }
    auto prepared = impl_->store->prepare(session_root);
    if (!prepared)
    {
        impl_->degrade(prepared.error());
        std::scoped_lock lock{impl_->mutex};
        impl_->completed = true;
        return Result<void>::success();
    }
    auto indexed = impl_->index->prepare(session_root, impl_->maximum_index_blocks,
                                         nvme_default_maximum_event_leases);
    if (!indexed)
    {
        impl_->degrade(indexed.error());
        std::scoped_lock lock{impl_->mutex};
        impl_->completed = true;
        return Result<void>::success();
    }
    auto index_snapshot = impl_->index->snapshot();
    {
        std::scoped_lock lock{impl_->mutex};
        impl_->metrics.current_cache_bytes = 0U;
        impl_->apply_index_snapshot_locked(index_snapshot);
    }
    if (auto reclaimed = impl_->reclaim_for(0U); !reclaimed)
    {
        impl_->degrade(reclaimed.error());
        std::scoped_lock lock{impl_->mutex};
        impl_->completed = true;
        return Result<void>::success();
    }
    {
        std::scoped_lock lock{impl_->mutex};
        impl_->metrics.state = NvmeCacheState::running;
        impl_->metrics.accepting = true;
    }
    try
    {
        impl_->worker =
            std::jthread([state = impl_.get()](const std::stop_token token) { state->run(token); });
    }
    catch (const std::exception& exception)
    {
        auto error = nvme_error("NVME_CACHE_UNAVAILABLE", Severity::error, "无法创建 NVMe 写入线程",
                                "storage.nvme.start", "thread-create-failed", true);
        error.details.push_back({"exception", exception.what()});
        impl_->degrade(error);
        std::scoped_lock lock{impl_->mutex};
        impl_->completed = true;
    }
    return Result<void>::success();
}

Result<NvmeEventLeaseOutcome> NvmeRollingCache::protect_event_window(NvmeEventLeaseRequest request)
{
    std::scoped_lock index_lock{impl_->index_operation_mutex};
    auto protected_window = impl_->index->protect_event_window(std::move(request));
    auto index_snapshot = impl_->index->snapshot();
    {
        std::scoped_lock lock{impl_->mutex};
        if (!protected_window)
        {
            ++impl_->metrics.lease_failures;
            impl_->metrics.last_error = protected_window.error();
        }
        impl_->apply_index_snapshot_locked(index_snapshot);
    }
    return protected_window;
}

Result<void> NvmeRollingCache::release_event(const std::string_view event_id)
{
    std::scoped_lock index_lock{impl_->index_operation_mutex};
    auto released = impl_->index->release_event(event_id);
    auto index_snapshot = impl_->index->snapshot();
    {
        std::scoped_lock lock{impl_->mutex};
        if (!released)
        {
            ++impl_->metrics.lease_failures;
            impl_->metrics.last_error = released.error();
        }
        impl_->apply_index_snapshot_locked(index_snapshot);
    }
    return released;
}

Result<NvmeFrameSequenceTrace> NvmeRollingCache::trace_window(
    const NvmeBlockWindowQuery& query) const
{
    std::scoped_lock index_lock{impl_->index_operation_mutex};
    return impl_->index->trace_window(query);
}

Result<NvmeSubmitStatus> NvmeRollingCache::submit_frame(camera::FrameView frame)
{
    bool notify_queue_full{};
    Error queue_error;
    NvmeSubmitStatus status{NvmeSubmitStatus::accepted};
    {
        std::scoped_lock lock{impl_->mutex};
        if (impl_->metrics.state == NvmeCacheState::memory_degraded)
            return Result<NvmeSubmitStatus>::success(NvmeSubmitStatus::memory_degraded);
        if (!impl_->started || impl_->stop_requested)
            return Result<NvmeSubmitStatus>::success(NvmeSubmitStatus::closed);
        if (!impl_->ordinary_writes_allowed_locked())
            return Result<NvmeSubmitStatus>::success(NvmeSubmitStatus::watermark_blocked);

        const auto found = impl_->lanes.find(frame.camera_id());
        if (found == impl_->lanes.end())
        {
            auto error =
                nvme_error("NVME_BLOCK_INVALID", Severity::warning, "NVMe 写入器拒绝未配置相机帧",
                           "storage.nvme.submit", "unknown-camera");
            error.source_id = frame.camera_id();
            return Result<NvmeSubmitStatus>::failure(std::move(error));
        }
        auto& lane = found->second;
        if (frame.bytes().size() > lane.layout.maximum_frame_bytes)
        {
            auto error =
                nvme_error("NVME_BLOCK_INVALID", Severity::error, "帧负载超过 NVMe 块声明上限",
                           "storage.nvme.submit", "frame-too-large");
            error.source_id = frame.camera_id();
            return Result<NvmeSubmitStatus>::failure(std::move(error));
        }
        if (lane.last_sequence)
        {
            if (frame.sequence_number() <= *lane.last_sequence ||
                (lane.last_monotonic && frame.received_monotonic_time() <= *lane.last_monotonic))
            {
                auto error = nvme_error("NVME_BLOCK_INVALID", Severity::warning,
                                        "NVMe 块帧序号或单调时间不是严格递增",
                                        "storage.nvme.submit", "frame-order-not-increasing");
                error.source_id = frame.camera_id();
                return Result<NvmeSubmitStatus>::failure(std::move(error));
            }
            if (frame.sequence_number() > *lane.last_sequence + 1U)
                impl_->metrics.sequence_gaps =
                    saturating_add(impl_->metrics.sequence_gaps,
                                   frame.sequence_number() - *lane.last_sequence - 1U);
        }
        lane.last_sequence = frame.sequence_number();
        lane.last_monotonic = frame.received_monotonic_time();
        ++impl_->metrics.submitted_frames;

        if (lane.current && frame.received_monotonic_time() - lane.current->start_monotonic_time >=
                                nvme_block_duration)
        {
            if (!impl_->enqueue_current_locked(lane))
            {
                status = NvmeSubmitStatus::queue_full;
                notify_queue_full = true;
            }
            else
            {
                status = NvmeSubmitStatus::block_enqueued;
            }
        }
        if (!lane.current)
            lane.current = impl_->new_block(lane, frame);
        lane.current->frames.push_back(std::move(frame));
        if (lane.current->frames.size() == lane.layout.index_capacity)
        {
            if (!impl_->enqueue_current_locked(lane))
            {
                status = NvmeSubmitStatus::queue_full;
                notify_queue_full = true;
            }
            else
            {
                status = NvmeSubmitStatus::block_enqueued;
            }
        }
        if (notify_queue_full)
        {
            queue_error = nvme_error("NVME_QUEUE_FULL", Severity::warning,
                                     "NVMe 有界块队列已满，普通缓存形成缺口",
                                     "storage.nvme.enqueue", "per-camera-capacity", true);
            queue_error.source_id = lane.layout.camera_id;
            impl_->metrics.last_error = queue_error;
        }
    }
    impl_->condition.notify_one();
    if (notify_queue_full)
        impl_->notify_error(queue_error);
    return Result<NvmeSubmitStatus>::success(status);
}

void NvmeRollingCache::set_storage_watermark(const StorageWatermark watermark) noexcept
{
    std::scoped_lock lock{impl_->mutex};
    impl_->metrics.storage_watermark = watermark;
    impl_->metrics.event_writes_allowed = watermark != StorageWatermark::stop_save;
    if (impl_->metrics.state == NvmeCacheState::memory_degraded || impl_->stop_requested)
        return;
    const bool allowed =
        watermark == StorageWatermark::normal || watermark == StorageWatermark::warning;
    impl_->metrics.state = allowed ? NvmeCacheState::running : NvmeCacheState::watermark_blocked;
    impl_->metrics.accepting = allowed;
    if (!allowed)
        impl_->discard_open_blocks_locked();
}

void NvmeRollingCache::request_stop() noexcept
{
    {
        std::scoped_lock lock{impl_->mutex};
        if (!impl_->started || impl_->stop_requested)
            return;
        const bool drain_open_blocks = impl_->ordinary_writes_allowed_locked();
        impl_->stop_requested = true;
        impl_->metrics.accepting = false;
        if (impl_->metrics.state != NvmeCacheState::memory_degraded)
            impl_->metrics.state = NvmeCacheState::stopping;
        if (drain_open_blocks)
        {
            for (auto& [unused, lane] : impl_->lanes)
            {
                static_cast<void>(unused);
                static_cast<void>(impl_->enqueue_current_locked(lane));
            }
        }
        else
        {
            impl_->discard_open_blocks_locked();
        }
    }
    impl_->condition.notify_all();
}

Result<void> NvmeRollingCache::join(const std::chrono::steady_clock::time_point deadline)
{
    std::unique_lock lock{impl_->mutex};
    if (!impl_->started || impl_->completed)
    {
        lock.unlock();
        if (impl_->worker.joinable())
            impl_->worker.join();
        return Result<void>::success();
    }
    if (!impl_->condition.wait_until(lock, deadline, [this] { return impl_->completed; }))
    {
        lock.unlock();
        impl_->worker.request_stop();
        impl_->store->cancel_pending();
        impl_->condition.notify_all();
        return Result<void>::failure(nvme_error("SYS_SHUTDOWN_TIMEOUT", Severity::critical,
                                                "NVMe 写入线程未在截止时间内停止",
                                                "storage.nvme.join", "deadline-exceeded", true));
    }
    lock.unlock();
    if (impl_->worker.joinable())
        impl_->worker.join();
    return Result<void>::success();
}

NvmeRollingCacheSnapshot NvmeRollingCache::snapshot() const noexcept
{
    std::scoped_lock lock{impl_->mutex};
    return impl_->metrics;
}

} // namespace paperbreak::storage
