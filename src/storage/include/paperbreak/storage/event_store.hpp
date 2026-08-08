#pragma once

#include "paperbreak/camera/frame.hpp"
#include "paperbreak/common/result.hpp"
#include "paperbreak/common/threading.hpp"
#include "paperbreak/event/event_window.hpp"
#include "paperbreak/event/key_frame.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace paperbreak::storage
{

inline constexpr std::uint32_t event_manifest_schema_version = 1U;
inline constexpr std::size_t event_persist_default_capacity = 8U;

struct EventManifestMetadata final
{
    std::string event_id;
    std::string event_state;
    camera::WallClockTime candidate_time;
    std::optional<camera::WallClockTime> confirmed_time;
    camera::WallClockTime start_time;
    camera::WallClockTime end_time;
    std::vector<std::string> camera_ids;
    std::string trigger_camera_id;
    std::uint64_t trigger_frame_number{};
    std::string trigger_reason;
    double confidence{};
    std::chrono::milliseconds pre_event_duration{};
    std::chrono::milliseconds post_event_duration{};
    std::string algorithm_name;
    std::string algorithm_version;
    std::string config_version;
    std::string machine_id;
    std::string production_line_id;
    std::string paper_type;
    std::optional<double> paper_speed;
    std::string upload_state{"Pending"};
    std::string time_quality{"Normal"};
};

struct PersistedKeyFrame final
{
    event::KeyFrameDescriptor descriptor;
    std::vector<std::byte> jpeg;
};

struct EventPersistenceRequest final
{
    EventManifestMetadata metadata;
    event::FrozenEventWindow window;
    std::vector<PersistedKeyFrame> key_frames;
};

enum class EventPathKind
{
    missing,
    regular_file,
    directory,
    other,
};

/// Injectable, bounded file-system boundary used by event transactions and recovery tests.
class IEventFileSystem
{
  public:
    virtual ~IEventFileSystem() = default;

    [[nodiscard]] virtual Result<void> create_directories(const std::filesystem::path& path) = 0;
    [[nodiscard]] virtual Result<void> create_directory_exclusive(
        const std::filesystem::path& path) = 0;
    [[nodiscard]] virtual Result<void> write_new_file_durable(
        const std::filesystem::path& path, std::span<const std::byte> contents) = 0;
    [[nodiscard]] virtual Result<std::vector<std::byte>> read_file_bounded(
        const std::filesystem::path& path, std::size_t maximum_bytes) = 0;
    [[nodiscard]] virtual Result<EventPathKind> path_kind(const std::filesystem::path& path) = 0;
    [[nodiscard]] virtual Result<std::vector<std::filesystem::path>> list_directories_bounded(
        const std::filesystem::path& path, std::size_t maximum_entries) = 0;
    /// Moves a directory without replacing the destination. Implementations must reject
    /// cross-volume moves and make the rename durable to the extent supported by the platform.
    [[nodiscard]] virtual Result<void> move_directory_atomically(
        const std::filesystem::path& source, const std::filesystem::path& destination) = 0;
};

/// Creates the Windows implementation using CREATE_NEW, FlushFileBuffers, and MoveFileExW.
[[nodiscard]] std::shared_ptr<IEventFileSystem> make_windows_event_file_system();

struct EventStoreOptions final
{
    std::filesystem::path event_root;
    std::size_t maximum_raw_frames{262144U};
    std::size_t maximum_key_frames{event::key_frame_reason_count};
    std::size_t maximum_file_bytes{128U * 1024U * 1024U};
    std::size_t maximum_manifest_bytes{8U * 1024U * 1024U};
    std::size_t maximum_recovery_entries{1024U};
};

struct EventPersistenceOutcome final
{
    std::string event_id;
    std::filesystem::path committed_directory;
    std::filesystem::path transaction_directory;
    std::string manifest_json;
    std::size_t raw_file_count{};
    std::size_t key_frame_count{};
};

enum class EventRecoveryDisposition
{
    committed,
    quarantined,
};

struct EventRecoveryItem final
{
    std::filesystem::path original_directory;
    std::filesystem::path resulting_directory;
    EventRecoveryDisposition disposition{EventRecoveryDisposition::quarantined};
    std::string event_id;
    std::string reason;
};

struct EventRecoveryReport final
{
    std::size_t scanned{};
    std::size_t committed{};
    std::size_t quarantined{};
    std::vector<EventRecoveryItem> items;
};

class IEventTransactionWriter
{
  public:
    virtual ~IEventTransactionWriter() = default;

    [[nodiscard]] virtual Result<EventPersistenceOutcome> persist(
        const EventPersistenceRequest& request) = 0;
};

/// Synchronous transaction boundary. Call it only from a bounded storage worker.
class EventTransactionWriter final : public IEventTransactionWriter
{
  public:
    class ConstructionKey final
    {
      public:
        ConstructionKey(const ConstructionKey&) = default;

      private:
        friend class EventTransactionWriter;
        ConstructionKey() = default;
    };

    [[nodiscard]] static Result<std::unique_ptr<EventTransactionWriter>> create(
        EventStoreOptions options,
        std::shared_ptr<IEventFileSystem> file_system = make_windows_event_file_system());

    EventTransactionWriter(ConstructionKey, EventStoreOptions options,
                           std::shared_ptr<IEventFileSystem> file_system);
    ~EventTransactionWriter();
    EventTransactionWriter(const EventTransactionWriter&) = delete;
    EventTransactionWriter& operator=(const EventTransactionWriter&) = delete;
    EventTransactionWriter(EventTransactionWriter&&) = delete;
    EventTransactionWriter& operator=(EventTransactionWriter&&) = delete;

    [[nodiscard]] Result<EventPersistenceOutcome> persist(
        const EventPersistenceRequest& request) override;
    [[nodiscard]] Result<EventRecoveryReport> recover_pending();

    /// Reads and verifies an already committed directory. Internal transaction/quarantine paths
    /// and paths outside the configured root are rejected.
    [[nodiscard]] Result<std::string> verify_committed_manifest(
        const std::filesystem::path& committed_directory) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct EventPersistenceCompletion final
{
    std::string event_id;
    std::optional<EventPersistenceOutcome> outcome;
    std::optional<Error> error;
};

using EventPersistenceCallback = std::function<void(EventPersistenceCompletion)>;

struct EventPersistenceRuntimeOptions final
{
    std::size_t event_capacity{event_persist_default_capacity};
    ThreadRegistrationFactory register_thread;
    DebugDiagnosticSink diagnostics;
};

struct EventPersistenceRuntimeSnapshot final
{
    bool started{};
    bool accepting{};
    std::size_t depth{};
    std::size_t capacity{};
    std::size_t high_watermark{};
    std::uint64_t submitted{};
    std::uint64_t completed{};
    std::uint64_t rejected{};
    std::uint64_t write_failures{};
    std::uint64_t callback_failures{};
};

/// Fixed-capacity, single-worker event persistence runtime. Submission never waits for disk I/O.
class EventPersistenceRuntime final
{
  public:
    class ConstructionKey final
    {
      public:
        ConstructionKey(const ConstructionKey&) = default;

      private:
        friend class EventPersistenceRuntime;
        ConstructionKey() = default;
    };

    [[nodiscard]] static Result<std::unique_ptr<EventPersistenceRuntime>> create(
        std::unique_ptr<IEventTransactionWriter> writer, EventPersistenceCallback callback,
        EventPersistenceRuntimeOptions options = {});

    EventPersistenceRuntime(ConstructionKey, std::unique_ptr<IEventTransactionWriter> writer,
                            EventPersistenceCallback callback,
                            EventPersistenceRuntimeOptions options);
    ~EventPersistenceRuntime();
    EventPersistenceRuntime(const EventPersistenceRuntime&) = delete;
    EventPersistenceRuntime& operator=(const EventPersistenceRuntime&) = delete;
    EventPersistenceRuntime(EventPersistenceRuntime&&) = delete;
    EventPersistenceRuntime& operator=(EventPersistenceRuntime&&) = delete;

    [[nodiscard]] Result<void> start();
    [[nodiscard]] Result<void> submit(EventPersistenceRequest request);
    void request_stop() noexcept;
    [[nodiscard]] Result<void> join(camera::MonotonicTime deadline);
    [[nodiscard]] EventPersistenceRuntimeSnapshot snapshot() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace paperbreak::storage
