#pragma once

#include "paperbreak/common/result.hpp"
#include "paperbreak/uplink/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;

namespace paperbreak::uplink::simulator
{

struct WorkspaceReport final
{
    std::uint64_t used_bytes{};
    std::size_t device_count{};
    std::size_t recovered_uploads{};
    std::size_t quarantined_uploads{};
};

struct StoredSession final
{
    std::string session_id;
    std::string machine_id;
    std::string response_json;
    bool duplicate{};
};

struct StoredUpload final
{
    std::string upload_id;
    std::string response_json;
    bool duplicate{};
};

struct StoredUploadSnapshot final
{
    std::string upload_id;
    std::string machine_id;
    std::string event_id;
    std::string logical_file_id;
    std::string state;
    std::uint64_t received_bytes{};
    std::uint64_t total_bytes{};
};

class Workspace final
{
  public:
    Workspace();
    ~Workspace();
    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;

    [[nodiscard]] Result<WorkspaceReport> open(std::filesystem::path root,
                                               std::uint64_t limit_bytes,
                                               std::size_t maximum_device_count,
                                               std::stop_token stop_token = {});
    void close() noexcept;
    [[nodiscard]] std::uint64_t used_bytes() const noexcept;

    [[nodiscard]] Result<StoredSession> create_session(const SessionHello& hello,
                                                       std::string session_id,
                                                       std::string server_time,
                                                       std::string websocket_host,
                                                       std::uint16_t port);
    [[nodiscard]] Result<bool> has_session(std::string_view session_id,
                                           std::string_view machine_id);
    [[nodiscard]] Result<std::string> session_machine(std::string_view session_id);
    [[nodiscard]] Result<void> close_session(std::string_view session_id,
                                             std::string_view timestamp);
    [[nodiscard]] Result<void> store_message(const MessageEnvelope& envelope);
    [[nodiscard]] Result<std::string> store_event(std::string_view machine_id,
                                                  std::string_view event_id,
                                                  std::string_view request_json);
    [[nodiscard]] Result<StoredUpload> create_upload(std::string_view machine_id,
                                                     const UploadCreateRequest& request,
                                                     std::string upload_id);
    [[nodiscard]] Result<std::string> upload_status(std::string_view machine_id,
                                                    std::string_view upload_id);
    [[nodiscard]] Result<std::size_t> active_upload_count();
    [[nodiscard]] Result<std::vector<StoredUploadSnapshot>> upload_snapshots();
    [[nodiscard]] Result<std::string> store_chunk(
        std::string_view machine_id, std::string_view upload_id, std::uint32_t chunk_index,
        std::string_view expected_sha256, std::string_view content_range,
        std::span<const std::byte> bytes, bool force_mismatch);
    [[nodiscard]] Result<std::string> complete_upload(std::string_view machine_id,
                                                      std::string_view upload_id,
                                                      bool force_mismatch);
    [[nodiscard]] Result<void> store_command(std::string_view command_id,
                                             std::string_view machine_id,
                                             std::string_view command_type,
                                             std::string_view payload_json,
                                             std::string_view deadline);
    [[nodiscard]] Result<void> complete_command(std::string_view command_id,
                                                std::string_view result_json);

  private:
    struct DatabaseDeleter final
    {
        void operator()(sqlite3* database) const noexcept;
    };
    std::unique_ptr<sqlite3, DatabaseDeleter> database_;
    std::filesystem::path root_;
    std::uint64_t limit_bytes_{};
    std::uint64_t used_bytes_{};
    std::size_t maximum_device_count_{};
    std::stop_token stop_token_;
};

} // namespace paperbreak::uplink::simulator
