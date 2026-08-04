#pragma once

#include "paperbreak/common/result.hpp"
#include "paperbreak/ipc/client.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace paperbreak::console
{

struct EventConfigurationValue final
{
    std::uint32_t pre_event_seconds{10U};
    std::uint32_t post_event_seconds{10U};
    std::uint32_t max_event_seconds{60U};
    std::uint32_t merge_gap_seconds{3U};
    std::uint32_t key_frame_count{7U};
    bool save_raw{true};
    bool generate_preview_video{};
    std::string upload_policy{"confirmed"};
    std::uint32_t retention_days{30U};
};

struct EventListFilter final
{
    std::optional<std::int64_t> start_time_utc_ms;
    std::optional<std::int64_t> end_time_utc_ms;
    std::optional<std::string> event_state;
    std::optional<std::string> camera_id;
    std::size_t offset{};
    std::size_t limit{50U};
};

struct EventListItem final
{
    std::string event_id;
    std::string event_state;
    std::uint64_t review_revision{};
    std::int64_t candidate_time_utc_ms{};
    std::string trigger_camera_id;
    double confidence{};
    std::string upload_state;
    std::string storage_state;
    bool thumbnail_available{};
};

struct EventDetail final
{
    EventListItem event;
    std::filesystem::path committed_directory;
    std::string manifest_json;
    std::vector<std::byte> thumbnail_jpeg;
    std::size_t raw_frame_count{};
    std::size_t key_frame_count{};
    std::uint64_t observed_sequence_gaps{};
    bool key_frames_traceable{};
};

struct EventClientSnapshot final
{
    ipc::ClientConnectionSnapshot connection;
    EventConfigurationValue configuration;
    std::uint64_t stored_config_revision{};
    bool preview_video_generation_available{};
    bool upload_runtime_available{};
    bool configuration_stale{true};
    std::optional<Error> configuration_error;
    EventListFilter filter;
    std::vector<EventListItem> events;
    std::size_t total{};
    bool events_stale{true};
    std::optional<EventDetail> detail;
    bool operation_pending{};
    std::string operation;
    std::optional<std::filesystem::path> exported_path;
    std::optional<Error> error;
};

using EventClientObserver = std::function<void(const EventClientSnapshot&)>;

class EventClient final
{
  public:
    explicit EventClient(EventClientObserver observer = {}, ipc::IpcClientOptions options = {});
    ~EventClient();
    EventClient(const EventClient&) = delete;
    EventClient& operator=(const EventClient&) = delete;

    [[nodiscard]] Result<void> start();
    void stop() noexcept;
    void refresh();
    [[nodiscard]] Result<void> query(EventListFilter filter);
    [[nodiscard]] Result<void> get(std::string event_id);
    [[nodiscard]] Result<void> update_configuration(EventConfigurationValue value);
    [[nodiscard]] Result<void> manual_trigger(std::string camera_id);
    [[nodiscard]] Result<void> review(std::string event_id, std::uint64_t expected_revision,
                                      bool confirmed);
    [[nodiscard]] Result<void> export_event(std::string event_id,
                                            std::filesystem::path destination);
    [[nodiscard]] const EventClientSnapshot& snapshot() const noexcept;

  private:
    class FileExporter;
    void connection_changed(const ipc::ClientConnectionSnapshot& connection);
    [[nodiscard]] Result<void> send_operation(std::string command, std::string payload_json,
                                              std::filesystem::path destination = {});
    void config_completed(ipc::ClientRequestHandle handle, Result<ipc::ResponseMessage> result);
    void list_completed(ipc::ClientRequestHandle handle, Result<ipc::ResponseMessage> result);
    void detail_completed(ipc::ClientRequestHandle handle, Result<ipc::ResponseMessage> result);
    void manifest_completed(ipc::ClientRequestHandle handle, Result<ipc::ResponseMessage> result);
    void operation_completed(ipc::ClientRequestHandle handle, Result<ipc::ResponseMessage> result,
                             std::filesystem::path destination);
    void notify() const noexcept;

    EventClientObserver observer_;
    EventClientSnapshot snapshot_;
    std::unique_ptr<ipc::IpcClient> client_;
    std::unique_ptr<FileExporter> exporter_;
    std::shared_ptr<std::atomic_bool> alive_;
    std::optional<ipc::ClientRequestHandle> config_request_;
    std::optional<ipc::ClientRequestHandle> list_request_;
    std::optional<ipc::ClientRequestHandle> detail_request_;
    std::optional<ipc::ClientRequestHandle> manifest_request_;
    std::optional<ipc::ClientRequestHandle> operation_request_;
};

} // namespace paperbreak::console
