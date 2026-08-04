#pragma once

#include "paperbreak/camera/frame.hpp"
#include "paperbreak/common/result.hpp"
#include "paperbreak/config/basic_config.hpp"
#include "paperbreak/storage/metadata_database.hpp"
#include "paperbreak/storage/storage_policy.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>

namespace paperbreak::service
{

inline constexpr std::size_t event_frame_queue_default_capacity = 64U;

struct EventRuntimeOptions final
{
    config::EdgeConfig configuration;
    std::filesystem::path event_root;
    std::shared_ptr<storage::EventMetadataDatabase> database;
    std::shared_ptr<storage::StoragePolicyManager> storage_policy;
    std::size_t frame_queue_capacity{event_frame_queue_default_capacity};
    std::size_t persistence_capacity{8U};
    std::function<void(const Error&)> error_observer;
    std::function<void(const storage::EventMetadataRecord&)> committed_observer;
};

struct EventRuntimeSnapshot final
{
    bool started{};
    bool accepting{};
    std::size_t frame_queue_depth{};
    std::size_t frame_queue_capacity{};
    std::size_t frame_queue_high_watermark{};
    std::size_t pending_events{};
    std::uint64_t submitted_frames{};
    std::uint64_t processed_frames{};
    std::uint64_t rejected_frames{};
    std::uint64_t detector_failures{};
    std::uint64_t events_started{};
    std::uint64_t events_frozen{};
    std::uint64_t events_committed{};
    std::uint64_t event_failures{};
};

/// M5 service composition for the bounded in-memory event chain. Camera observers only call
/// submit_frame(), which never waits for encoding, SQLite, or disk I/O.
class EventRuntime final
{
  public:
    class ConstructionKey final
    {
      public:
        ConstructionKey(const ConstructionKey&) = default;

      private:
        friend class EventRuntime;
        ConstructionKey() = default;
    };

    [[nodiscard]] static Result<std::shared_ptr<EventRuntime>> create(EventRuntimeOptions options);

    EventRuntime(ConstructionKey, std::unique_ptr<struct EventRuntimeImpl> impl);
    ~EventRuntime();
    EventRuntime(const EventRuntime&) = delete;
    EventRuntime& operator=(const EventRuntime&) = delete;
    EventRuntime(EventRuntime&&) = delete;
    EventRuntime& operator=(EventRuntime&&) = delete;

    [[nodiscard]] Result<void> start();
    [[nodiscard]] Result<void> submit_frame(camera::FrameView frame);
    [[nodiscard]] Result<bool> request_manual_trigger(std::string_view camera_id);
    [[nodiscard]] Result<void> reconfigure(const config::EdgeConfig& configuration);
    void request_stop() noexcept;
    [[nodiscard]] Result<void> join(std::chrono::steady_clock::time_point deadline);
    [[nodiscard]] EventRuntimeSnapshot snapshot() const noexcept;

  private:
    std::unique_ptr<struct EventRuntimeImpl> impl_;
};

} // namespace paperbreak::service
