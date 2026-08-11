#pragma once

#include "paperbreak/common/result.hpp"
#include "paperbreak/ipc/client.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace paperbreak::console
{

struct AlgorithmRoiValue final
{
    std::uint32_t width{1U};
    std::uint32_t height{1U};
    std::uint32_t offset_x{};
    std::uint32_t offset_y{};
};

struct AlgorithmConfigurationValue final
{
    bool enabled{};
    std::string type{"mock"};
    AlgorithmRoiValue roi;
    double candidate_threshold{0.6};
    double confirmation_threshold{0.8};
    std::uint32_t consecutive_frames{3U};
    std::uint32_t cooldown_ms{1000U};
    std::string model_reference;
    std::string model_version;
    std::string device{"cpu"};
    bool debug_overlay{};
};

struct AlgorithmMetricValue final
{
    std::uint64_t queue_depth{};
    std::uint64_t queue_capacity{};
    std::uint64_t queue_high_watermark{};
    std::uint64_t submitted_frames{};
    std::uint64_t processed_frames{};
    std::uint64_t skipped_frames{};
    std::uint64_t detector_failures{};
    std::uint64_t consecutive_detector_failures{};
    std::uint64_t consecutive_backlog_events{};
    bool backlog_active{};
    std::uint64_t consecutive_bad_backlog_windows{};
    std::uint64_t consecutive_healthy_backlog_windows{};
    std::uint64_t result_queue_rejected{};
    std::uint64_t process_calls{};
    std::int64_t last_processing_time_us{};
    std::int64_t average_processing_time_us{};
    std::int64_t maximum_processing_time_us{};
    std::int64_t last_queue_wait_time_us{};
    std::int64_t average_queue_wait_time_us{};
    std::int64_t maximum_queue_wait_time_us{};
    std::int64_t last_end_to_end_time_us{};
    std::int64_t average_end_to_end_time_us{};
    std::int64_t maximum_end_to_end_time_us{};
    double input_fps{};
    double processed_fps{};
    double skipped_ratio{};
    std::uint64_t candidates_created{};
    std::uint64_t confirmed_events{};
    std::uint64_t rejected_candidates{};
};

struct AlgorithmRuntimeValue final
{
    std::string camera_id;
    std::uint64_t config_revision{};
    std::string state{"disabled"};
    bool has_current_frame{};
    std::uint64_t latest_sequence_number{};
    std::string plugin_id;
    std::string display_name;
    std::string implementation_version;
    std::string detector_model_version;
    bool supports_hot_update{};
    bool prototype_only{true};
    AlgorithmMetricValue metrics;
};

struct AlgorithmDebugMetricValue final
{
    std::string name;
    double value{};
};

struct AlgorithmTestResultValue final
{
    bool isolated{};
    bool candidate_created{};
    bool triggered{};
    bool anomalous{};
    std::string trigger_source;
    std::string candidate_type;
    std::uint64_t sequence_number{};
    double confidence{};
    double area_ratio{};
    double change_score{};
    std::int64_t processing_time_us{};
    std::string reason;
    std::string detector_version;
    std::string model_version;
    AlgorithmRoiValue evaluated_region;
    std::uint32_t preview_source_width{};
    std::uint32_t preview_source_height{};
    std::vector<AlgorithmDebugMetricValue> debug_metrics;
    std::vector<std::byte> preview_jpeg;
};

struct AlgorithmClientSnapshot final
{
    ipc::ClientConnectionSnapshot connection;
    std::string camera_id{"CAM01"};
    AlgorithmConfigurationValue configuration;
    AlgorithmConfigurationValue effective_configuration;
    std::uint64_t stored_config_revision{};
    std::uint64_t effective_config_revision{};
    AlgorithmRuntimeValue runtime;
    bool stale{true};
    bool operation_pending{};
    std::string operation;
    std::optional<AlgorithmTestResultValue> test_result;
    std::optional<Error> error;
};

using AlgorithmClientObserver = std::function<void(const AlgorithmClientSnapshot&)>;

class AlgorithmClient final
{
  public:
    explicit AlgorithmClient(AlgorithmClientObserver observer = {},
                             ipc::IpcClientOptions options = {});
    ~AlgorithmClient();
    AlgorithmClient(const AlgorithmClient&) = delete;
    AlgorithmClient& operator=(const AlgorithmClient&) = delete;

    [[nodiscard]] Result<void> start();
    void stop() noexcept;
    [[nodiscard]] Result<void> select_camera(std::string camera_id);
    void refresh();
    [[nodiscard]] Result<void> update_configuration(AlgorithmConfigurationValue value);
    [[nodiscard]] Result<void> test_current_frame();
    [[nodiscard]] const AlgorithmClientSnapshot& snapshot() const noexcept;

  private:
    void connection_changed(const ipc::ClientConnectionSnapshot& connection);
    void config_completed(ipc::ClientRequestHandle handle, Result<ipc::ResponseMessage> result);
    void operation_completed(ipc::ClientRequestHandle handle, Result<ipc::ResponseMessage> result);
    void notify() const noexcept;

    AlgorithmClientObserver observer_;
    AlgorithmClientSnapshot snapshot_;
    std::unique_ptr<ipc::IpcClient> client_;
    std::optional<ipc::ClientRequestHandle> config_request_;
    std::string config_request_camera_id_;
    std::optional<ipc::ClientRequestHandle> operation_request_;
};

} // namespace paperbreak::console
