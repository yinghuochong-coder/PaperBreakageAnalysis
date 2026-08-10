#pragma once

#include "paperbreak/common/result.hpp"
#include "paperbreak/storage/event_store.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace paperbreak::storage
{

struct InspectedRawFrame final
{
    std::filesystem::path relative_path;
    std::string camera_id;
    std::uint64_t camera_frame_number{};
    std::uint64_t sequence_number{};
    std::int64_t wall_clock_time_utc_ms{};
};

struct InspectedKeyFrame final
{
    std::filesystem::path relative_path;
    std::string camera_id;
    std::uint64_t camera_frame_number{};
    std::uint64_t sequence_number{};
    std::vector<std::string> reasons;
};

struct EventInspectionReport final
{
    std::string event_id;
    std::filesystem::path committed_directory;
    std::string manifest_json;
    std::vector<InspectedRawFrame> raw_frames;
    std::vector<InspectedKeyFrame> key_frames;
    std::vector<std::byte> thumbnail_jpeg;
    std::uint64_t observed_sequence_gaps{};
    bool key_frames_traceable{};
};

struct EventInspectionSummary final
{
    std::string event_id;
    std::filesystem::path committed_directory;
    std::size_t manifest_bytes{};
    std::size_t raw_frame_count{};
    std::size_t key_frame_count{};
    std::vector<std::byte> thumbnail_jpeg;
    std::uint64_t observed_sequence_gaps{};
    bool key_frames_traceable{};
};

struct EventInspectorOptions final
{
    std::filesystem::path event_root;
    std::size_t maximum_manifest_bytes{8U * 1024U * 1024U};
    std::size_t maximum_file_bytes{128U * 1024U * 1024U};
    std::size_t maximum_files{262144U};
    std::size_t maximum_export_bytes{16U * 1024U * 1024U};
    std::uint64_t maximum_file_export_bytes{64ULL * 1024ULL * 1024ULL * 1024ULL};
};

struct EventExportArchive final
{
    std::string event_id;
    std::string file_name;
    std::size_t source_file_count{};
    std::vector<std::byte> zip;
};

struct EventExportFile final
{
    std::string event_id;
    std::string file_name;
    std::size_t source_file_count{};
    std::uint64_t size_bytes{};
    std::filesystem::path path;
};

/// Read-only inspector for atomically committed events. Every entry is verified against the
/// immutable manifest before details, thumbnails, or export bytes are returned.
class EventInspector final
{
  public:
    class ConstructionKey final
    {
      public:
        ConstructionKey(const ConstructionKey&) = default;

      private:
        friend class EventInspector;
        ConstructionKey() = default;
    };

    [[nodiscard]] static Result<std::unique_ptr<EventInspector>> create(
        EventInspectorOptions options,
        std::shared_ptr<IEventFileSystem> file_system = make_windows_event_file_system());

    EventInspector(ConstructionKey, EventInspectorOptions options,
                   std::shared_ptr<IEventFileSystem> file_system);
    ~EventInspector();
    EventInspector(const EventInspector&) = delete;
    EventInspector& operator=(const EventInspector&) = delete;
    EventInspector(EventInspector&&) = delete;
    EventInspector& operator=(EventInspector&&) = delete;

    [[nodiscard]] Result<EventInspectionReport> inspect(
        const std::filesystem::path& committed_relative_directory) const;
    /// Builds the detail-view summary from the structurally checked manifest and verifies only the
    /// key-frame bytes returned as the thumbnail. Raw blocks and event.json are not read.
    [[nodiscard]] Result<EventInspectionSummary> inspect_summary(
        const std::filesystem::path& committed_relative_directory) const;
    /// Parses the manifest and checks path constraints, file existence, and declared lengths.
    /// This structural check never reads an event payload.
    [[nodiscard]] Result<std::string> get_manifest(
        const std::filesystem::path& committed_relative_directory) const;
    [[nodiscard]] Result<EventExportArchive> export_zip(
        const std::filesystem::path& committed_relative_directory) const;
    [[nodiscard]] Result<EventExportFile> export_zip_file(
        const std::filesystem::path& committed_relative_directory,
        const std::filesystem::path& destination) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace paperbreak::storage
