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
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace paperbreak::storage
{

inline constexpr std::uint32_t event_manifest_schema_version = 3U;
inline constexpr std::uint32_t event_manifest_legacy_schema_version = 2U;
inline constexpr std::size_t event_persist_default_capacity = 8U;
inline constexpr std::size_t event_raw_block_maximum_frames = 256U;
inline constexpr std::chrono::milliseconds event_raw_block_duration{1000};

struct EventManifestMetadata final
{
    std::string event_id;
    /// Compatibility input. New callers should set decision_state.
    std::string event_state;
    std::string decision_state;
    std::uint64_t trigger_count{1U};
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

struct BufferedFileWriteResult final
{
    std::uint64_t bytes_written{};
    std::string sha256;
};

struct HashedFileContents final
{
    std::vector<std::byte> contents;
    std::string sha256;
};

/// Injectable, bounded file-system boundary used by event transactions and recovery tests.
class IEventFileSystem
{
  public:
    virtual ~IEventFileSystem() = default;

    [[nodiscard]] virtual Result<void> create_directories(const std::filesystem::path& path) = 0;
    [[nodiscard]] virtual Result<void> create_directory_exclusive(
        const std::filesystem::path& path) = 0;
    /// Writes a new file through the ordinary Windows cache and hashes each successfully written
    /// byte exactly once. No power-loss durability flush is requested.
    [[nodiscard]] virtual Result<BufferedFileWriteResult> write_new_file_buffered(
        const std::filesystem::path& path, std::span<const std::byte> contents,
        std::stop_token stop_token = {}) = 0;
    /// Writes a complete PBNVME2 block to a unique temporary file, closes it, then publishes the
    /// final name with a same-directory, non-replacing atomic rename.
    [[nodiscard]] virtual Result<BufferedFileWriteResult> write_new_raw_block_buffered(
        const std::filesystem::path& path, std::span<const std::byte> contents,
        std::stop_token stop_token = {}) = 0;
    [[nodiscard]] virtual Result<std::vector<std::byte>> read_file_bounded(
        const std::filesystem::path& path, std::size_t maximum_bytes) = 0;
    /// Reads one file once and computes SHA-256 from the same successfully read byte stream.
    [[nodiscard]] virtual Result<HashedFileContents> read_file_bounded_hashed(
        const std::filesystem::path& path, std::size_t maximum_bytes) = 0;
    [[nodiscard]] virtual Result<std::uint64_t> file_size(const std::filesystem::path& path) = 0;
    [[nodiscard]] virtual Result<EventPathKind> path_kind(const std::filesystem::path& path) = 0;
    [[nodiscard]] virtual Result<std::vector<std::filesystem::path>> list_directories_bounded(
        const std::filesystem::path& path, std::size_t maximum_entries) = 0;
    /// Moves a directory without replacing the destination. Implementations must reject
    /// cross-volume moves. The rename publishes into the OS namespace but is not power-loss
    /// durable.
    [[nodiscard]] virtual Result<void> move_directory_atomically(
        const std::filesystem::path& source, const std::filesystem::path& destination) = 0;
};

/// Creates the Windows implementation using CREATE_NEW, CNG SHA-256, and ordinary MoveFileExW.
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
    std::size_t raw_block_count{};
    std::size_t raw_frame_count{};
    /// Compatibility alias for raw_block_count.
    std::size_t raw_file_count{};
    std::size_t key_frame_count{};
    std::uint64_t bytes_written{};
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
    [[nodiscard]] virtual Result<EventPersistenceOutcome> persist(
        const EventPersistenceRequest& request, std::stop_token stop_token)
    {
        if (stop_token.stop_requested())
            return Result<EventPersistenceOutcome>::failure(
                make_error("EVENT_WRITE_CANCELLED", Severity::warning, "事件持久化已取消",
                           "storage", "event.persist.cancel", true));
        return persist(request);
    }
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
    [[nodiscard]] Result<EventPersistenceOutcome> persist(const EventPersistenceRequest& request,
                                                          std::stop_token stop_token) override;
    [[nodiscard]] Result<EventRecoveryReport> recover_pending();

    /// Reads and structurally checks an already committed directory. It validates manifest shape,
    /// path constraints, file presence, and declared lengths without reading artifact contents.
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
    std::function<void(std::string_view)> writing_observer;
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
    std::size_t active_events{};
    std::uint64_t last_write_bytes{};
    std::chrono::milliseconds last_write_duration{};
    double last_write_mib_per_second{};
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
