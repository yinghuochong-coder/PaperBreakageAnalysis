#pragma once

#include "paperbreak/camera/frame.hpp"
#include "paperbreak/common/result.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace paperbreak::pipeline
{

inline constexpr std::size_t preview_maximum_cameras = 4U;
inline constexpr std::size_t preview_maximum_subscriptions = 4U;

struct PreviewRoi final
{
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t width{};
    std::uint32_t height{};
};

struct PreviewFrameMetadata final
{
    std::optional<double> brightness;
    std::optional<double> actual_fps;
    std::string camera_status;
    std::optional<PreviewRoi> roi;
    std::string detection_result;
};

struct PreviewEncodeOptions final
{
    std::uint32_t maximum_width{1280U};
    std::uint32_t maximum_height{720U};
    std::uint32_t jpeg_quality{80U};
    std::size_t maximum_binary_bytes{16U * 1024U * 1024U};
};

class IPreviewEncoder
{
  public:
    virtual ~IPreviewEncoder() = default;
    [[nodiscard]] virtual Result<std::vector<std::byte>> encode(
        const camera::FrameView& frame, const PreviewEncodeOptions& options) = 0;
};

[[nodiscard]] std::unique_ptr<IPreviewEncoder> make_opencv_preview_encoder();

struct PreviewDelivery final
{
    std::uint64_t subscriber_id{};
    std::string camera_id;
    std::uint64_t camera_frame_number{};
    std::uint64_t sequence_number{};
    camera::FrameGeometry source_geometry;
    PreviewFrameMetadata metadata;
    std::vector<std::byte> jpeg;
};

using PreviewDeliveryCallback = std::function<void(PreviewDelivery)>;

struct PreviewRuntimeOptions final
{
    double frames_per_second{3.0};
    PreviewEncodeOptions encoding;
    std::size_t maximum_cameras{preview_maximum_cameras};
    std::size_t maximum_subscriptions{preview_maximum_subscriptions};
};

struct PreviewRuntimeSnapshot final
{
    bool started{};
    std::size_t subscriptions{};
    std::uint64_t frames_received{};
    std::uint64_t frames_sampled{};
    std::uint64_t frames_skipped_without_subscribers{};
    std::uint64_t frames_skipped_by_rate{};
    std::uint64_t frames_replaced_before_encoding{};
    std::uint64_t encoded{};
    std::uint64_t encoding_failures{};
    std::uint64_t deliveries{};
    std::uint64_t delivery_failures{};
    std::uint64_t rejected_unknown_camera{};
    std::uint64_t rejected_after_stop{};
};

/// Best-effort low-rate preview branch. It never blocks the frame producer.
class PreviewRuntime final
{
  public:
    PreviewRuntime(std::vector<std::string> camera_ids, std::unique_ptr<IPreviewEncoder> encoder,
                   PreviewDeliveryCallback delivery, PreviewRuntimeOptions options = {});
    ~PreviewRuntime();

    PreviewRuntime(const PreviewRuntime&) = delete;
    PreviewRuntime& operator=(const PreviewRuntime&) = delete;

    [[nodiscard]] Result<void> start();
    void request_stop() noexcept;
    [[nodiscard]] Result<void> join(std::chrono::steady_clock::time_point deadline);
    [[nodiscard]] Result<void> subscribe(std::uint64_t subscriber_id,
                                         const std::vector<std::string>& camera_ids);
    void unsubscribe(std::uint64_t subscriber_id) noexcept;
    void submit(camera::FrameView frame, PreviewFrameMetadata metadata = {}) noexcept;
    [[nodiscard]] PreviewRuntimeSnapshot snapshot() const noexcept;

  private:
    struct PendingFrame final
    {
        camera::FrameView frame;
        PreviewFrameMetadata metadata;
    };
    struct CameraSlot;

    void run(std::stop_token token) noexcept;
    [[nodiscard]] bool has_subscriber_for(const std::string& camera_id) const noexcept;
    void finish() noexcept;

    std::unique_ptr<IPreviewEncoder> encoder_;
    PreviewDeliveryCallback delivery_;
    PreviewRuntimeOptions options_;
    std::unordered_map<std::string, std::unique_ptr<CameraSlot>> cameras_;
    mutable std::mutex subscriptions_mutex_;
    std::unordered_map<std::uint64_t, std::unordered_set<std::string>> subscriptions_;
    mutable std::mutex lifecycle_mutex_;
    std::condition_variable lifecycle_condition_;
    bool started_{};
    bool completed_{true};
    std::jthread worker_;
    std::atomic<std::uint64_t> frames_received_{};
    std::atomic<std::uint64_t> frames_sampled_{};
    std::atomic<std::uint64_t> frames_skipped_without_subscribers_{};
    std::atomic<std::uint64_t> frames_skipped_by_rate_{};
    std::atomic<std::uint64_t> frames_replaced_before_encoding_{};
    std::atomic<std::uint64_t> encoded_{};
    std::atomic<std::uint64_t> encoding_failures_{};
    std::atomic<std::uint64_t> deliveries_{};
    std::atomic<std::uint64_t> delivery_failures_{};
    std::atomic<std::uint64_t> rejected_unknown_camera_{};
    std::atomic<std::uint64_t> rejected_after_stop_{};
};

} // namespace paperbreak::pipeline
