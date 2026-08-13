#include "paperbreak/console/algorithm_metrics.hpp"

#include <QDateTime>
#include <QSaveFile>
#include <QString>
#include <QTimeZone>

#include <algorithm>
#include <array>
#include <deque>
#include <iomanip>
#include <locale>
#include <map>
#include <sstream>
#include <utility>

namespace paperbreak::console
{
namespace
{

template <auto Member> AlgorithmMetricScalar read_member(const AlgorithmMetricValue& value)
{
    return value.*Member;
}

AlgorithmMetricScalar read_skipped_ratio_percent(const AlgorithmMetricValue& value)
{
    return value.skipped_ratio * 100.0;
}

constexpr std::array metric_descriptors{
    AlgorithmMetricDescriptor{
        "submittedFrames", "提交帧数", "帧", AlgorithmMetricGroup::processing_throughput,
        "累计提交到当前相机算法通道的帧数；范围为本次服务运行期，运行时重建或服务重启后重置。",
        read_member<&AlgorithmMetricValue::submitted_frames>},
    AlgorithmMetricDescriptor{
        "processedFrames", "处理帧数", "帧", AlgorithmMetricGroup::processing_throughput,
        "累计完成算法处理的帧数；范围为当前相机和本次服务运行期，运行时重建或服务重启后重置。",
        read_member<&AlgorithmMetricValue::processed_frames>},
    AlgorithmMetricDescriptor{"processCalls", "处理调用数", "次",
                              AlgorithmMetricGroup::processing_throughput,
                              "检测器累计调用次数，包含成功与失败调用；范围为当前相机运行时，检测器"
                              "重建或服务重启后重置。",
                              read_member<&AlgorithmMetricValue::process_calls>},
    AlgorithmMetricDescriptor{
        "configuredProcessingFps", "配置处理速率", "FPS",
        AlgorithmMetricGroup::processing_throughput,
        "当前相机自动算法配置的目标处理节拍；这是瞬时配置值，配置生效时更新，不作累计。",
        read_member<&AlgorithmMetricValue::configured_processing_fps>},
    AlgorithmMetricDescriptor{
        "inputFps", "输入速率", "FPS", AlgorithmMetricGroup::processing_throughput,
        "最近统计窗口内进入算法调度的帧速率；范围为当前相机，窗口滚动更新，运行时重建后清零。",
        read_member<&AlgorithmMetricValue::input_fps>},
    AlgorithmMetricDescriptor{
        "processedFps", "处理速率", "FPS", AlgorithmMetricGroup::processing_throughput,
        "最近统计窗口内完成处理的帧速率；范围为当前相机，窗口滚动更新，运行时重建后清零。",
        read_member<&AlgorithmMetricValue::processed_fps>},

    AlgorithmMetricDescriptor{
        "lastProcessingTimeUs", "最近处理耗时", "µs", AlgorithmMetricGroup::latency_performance,
        "最近一次检测器调用的实测耗时；范围为当前相机最近有效调用，新调用覆盖，运行时重建后清零。",
        read_member<&AlgorithmMetricValue::last_processing_time_us>},
    AlgorithmMetricDescriptor{
        "averageProcessingTimeUs", "平均处理耗时", "µs", AlgorithmMetricGroup::latency_performance,
        "当前相机运行期全部检测器调用的累计平均耗时；检测器运行时重建或服务重启后重置。",
        read_member<&AlgorithmMetricValue::average_processing_time_us>},
    AlgorithmMetricDescriptor{
        "maximumProcessingTimeUs", "最大处理耗时", "µs", AlgorithmMetricGroup::latency_performance,
        "当前相机运行期检测器调用的最大耗时；检测器运行时重建或服务重启后重置。",
        read_member<&AlgorithmMetricValue::maximum_processing_time_us>},
    AlgorithmMetricDescriptor{
        "lastQueueWaitTimeUs", "最近队列等待", "µs", AlgorithmMetricGroup::latency_performance,
        "最近一帧从算法槽接收到开始处理的等待时间；当前相机单点值，新处理覆盖。",
        read_member<&AlgorithmMetricValue::last_queue_wait_time_us>},
    AlgorithmMetricDescriptor{
        "averageQueueWaitTimeUs", "平均队列等待", "µs", AlgorithmMetricGroup::latency_performance,
        "当前相机运行期已处理帧队列等待时间的累计平均值；运行时重建或服务重启后重置。",
        read_member<&AlgorithmMetricValue::average_queue_wait_time_us>},
    AlgorithmMetricDescriptor{
        "maximumQueueWaitTimeUs", "最大队列等待", "µs", AlgorithmMetricGroup::latency_performance,
        "当前相机运行期观测到的最大队列等待时间；运行时重建或服务重启后重置。",
        read_member<&AlgorithmMetricValue::maximum_queue_wait_time_us>},
    AlgorithmMetricDescriptor{
        "lastEndToEndTimeUs", "最近端到端帧龄", "µs", AlgorithmMetricGroup::latency_performance,
        "最近处理结果完成时相对帧接收时刻的端到端帧龄；当前相机单点值，新结果覆盖。",
        read_member<&AlgorithmMetricValue::last_end_to_end_time_us>},
    AlgorithmMetricDescriptor{
        "averageEndToEndTimeUs", "平均端到端帧龄", "µs", AlgorithmMetricGroup::latency_performance,
        "当前相机运行期已处理结果端到端帧龄的累计平均值；运行时重建或服务重启后重置。",
        read_member<&AlgorithmMetricValue::average_end_to_end_time_us>},
    AlgorithmMetricDescriptor{"maximumEndToEndTimeUs", "最大端到端帧龄", "µs",
                              AlgorithmMetricGroup::latency_performance,
                              "当前相机运行期观测到的最大端到端帧龄；运行时重建或服务重启后重置。",
                              read_member<&AlgorithmMetricValue::maximum_end_to_end_time_us>},

    AlgorithmMetricDescriptor{
        "queueDepth", "队列深度", "帧", AlgorithmMetricGroup::backlog_stability,
        "当前相机自动算法 latest-wins 槽的瞬时占用深度；每次快照重新读取，不累计。",
        read_member<&AlgorithmMetricValue::queue_depth>},
    AlgorithmMetricDescriptor{"queueCapacity", "队列容量", "帧",
                              AlgorithmMetricGroup::backlog_stability,
                              "当前相机算法输入通道的固定容量；运行时配置值，不随负载变化。",
                              read_member<&AlgorithmMetricValue::queue_capacity>},
    AlgorithmMetricDescriptor{"queueHighWatermark", "队列高水位", "帧",
                              AlgorithmMetricGroup::backlog_stability,
                              "当前相机运行期算法输入占用的最高值；运行时重建或服务重启后重置。",
                              read_member<&AlgorithmMetricValue::queue_high_watermark>},
    AlgorithmMetricDescriptor{
        "skippedFrames", "积压跳帧数", "帧", AlgorithmMetricGroup::backlog_stability,
        "累计因算法积压保护而未处理的帧数；不含正常节拍抽样，当前相机运行时重建后重置。",
        read_member<&AlgorithmMetricValue::skipped_frames>},
    AlgorithmMetricDescriptor{
        "sampledSkippedFrames", "正常抽样跳过", "帧", AlgorithmMetricGroup::backlog_stability,
        "累计因配置处理节拍而正常跳过的帧数；不表示积压，当前相机运行时重建后重置。",
        read_member<&AlgorithmMetricValue::sampled_skipped_frames>},
    AlgorithmMetricDescriptor{
        "missedProcessingSlots", "错过处理周期", "周期", AlgorithmMetricGroup::backlog_stability,
        "累计因检测调用超出配置周期而错过的处理节拍；当前相机运行时重建后重置。",
        read_member<&AlgorithmMetricValue::missed_processing_slots>},
    AlgorithmMetricDescriptor{
        "detectorFailures", "检测失败数", "次", AlgorithmMetricGroup::backlog_stability,
        "累计检测器处理失败次数；范围为当前相机运行时，检测器重建或服务重启后重置。",
        read_member<&AlgorithmMetricValue::detector_failures>},
    AlgorithmMetricDescriptor{
        "consecutiveDetectorFailures", "连续检测失败", "次",
        AlgorithmMetricGroup::backlog_stability,
        "当前连续失败次数；检测成功或检测器事务式重配置后归零，范围为当前相机。",
        read_member<&AlgorithmMetricValue::consecutive_detector_failures>},
    AlgorithmMetricDescriptor{
        "consecutiveBacklogEvents", "连续积压事件", "次", AlgorithmMetricGroup::backlog_stability,
        "当前连续积压观测次数；健康观测会中断并重置，范围为当前相机积压判定窗口。",
        read_member<&AlgorithmMetricValue::consecutive_backlog_events>},
    AlgorithmMetricDescriptor{"backlogActive", "积压活动", "状态",
                              AlgorithmMetricGroup::backlog_stability,
                              "当前相机是否处于算法积压活动状态；布尔瞬时值，积压清除后恢复为否。",
                              read_member<&AlgorithmMetricValue::backlog_active>},
    AlgorithmMetricDescriptor{"consecutiveBadBacklogWindows", "连续异常窗口", "窗口",
                              AlgorithmMetricGroup::backlog_stability,
                              "连续达到积压判定条件的统计窗口数；健康窗口会重置，范围为当前相机。",
                              read_member<&AlgorithmMetricValue::consecutive_bad_backlog_windows>},
    AlgorithmMetricDescriptor{
        "consecutiveHealthyBacklogWindows", "连续健康窗口", "窗口",
        AlgorithmMetricGroup::backlog_stability,
        "连续未达到积压条件的统计窗口数；异常窗口会重置，用于当前相机恢复判定。",
        read_member<&AlgorithmMetricValue::consecutive_healthy_backlog_windows>},
    AlgorithmMetricDescriptor{
        "resultQueueRejected", "结果队列拒绝", "次", AlgorithmMetricGroup::backlog_stability,
        "累计因全局算法结果入口满载而被拒绝的当前相机结果数；运行时重建或服务重启后重置。",
        read_member<&AlgorithmMetricValue::result_queue_rejected>},
    AlgorithmMetricDescriptor{
        "rearmPending", "等待重新布防", "状态", AlgorithmMetricGroup::backlog_stability,
        "当前相机是否仍需同时满足冷却和稳定正常画面条件；人工测试可显式绕过。",
        read_member<&AlgorithmMetricValue::rearm_pending>},
    AlgorithmMetricDescriptor{
        "rearmSuppressedResults", "重新布防抑制结果", "次", AlgorithmMetricGroup::backlog_stability,
        "累计在等待重新布防期间被正常抑制的非人工异常结果；不增加候选或窗口触发。",
        read_member<&AlgorithmMetricValue::rearm_suppressed_results>},
    AlgorithmMetricDescriptor{
        "skippedRatio", "窗口跳帧率", "%", AlgorithmMetricGroup::backlog_stability,
        "最近统计窗口内积压跳帧占比；范围为当前相机，窗口滚动更新，界面按百分比显示。",
        read_skipped_ratio_percent},

    AlgorithmMetricDescriptor{
        "candidatesCreated", "候选事件数", "个", AlgorithmMetricGroup::detection_results,
        "累计由当前相机算法结果创建的候选事件数；范围为本次运行期，运行时重建后重置。",
        read_member<&AlgorithmMetricValue::candidates_created>},
    AlgorithmMetricDescriptor{
        "confirmedEvents", "确认事件数", "个", AlgorithmMetricGroup::detection_results,
        "累计由当前相机结果推进为确认状态的事件数；范围为本次运行期，运行时重建后重置。",
        read_member<&AlgorithmMetricValue::confirmed_events>},
    AlgorithmMetricDescriptor{
        "rejectedCandidates", "拒绝候选数", "个", AlgorithmMetricGroup::detection_results,
        "累计被判定为拒绝的当前相机候选数；范围为本次运行期，运行时重建后重置。",
        read_member<&AlgorithmMetricValue::rejected_candidates>}};

constexpr std::array debug_descriptors{
    AlgorithmDebugMetricDescriptor{"meanGrayscale", "平均灰度", "归一化值",
                                   AlgorithmDebugMetricGroup::image_detection,
                                   "本次隔离测试 ROI 内像素的归一化平均灰度。"},
    AlgorithmDebugMetricDescriptor{"meanGrayscaleChange", "平均灰度变化", "归一化值",
                                   AlgorithmDebugMetricGroup::image_detection,
                                   "当前 ROI 平均灰度相对该隔离检测器上一帧的绝对变化。"},
    AlgorithmDebugMetricDescriptor{"paperRatio", "纸幅占比", "比例",
                                   AlgorithmDebugMetricGroup::image_detection,
                                   "本次分析区域中满足纸幅灰度条件的像素比例。"},
    AlgorithmDebugMetricDescriptor{"backgroundMeanChange", "背景平均变化", "归一化值",
                                   AlgorithmDebugMetricGroup::image_detection,
                                   "当前分析图像相对健康背景的归一化平均绝对差。"},
    AlgorithmDebugMetricDescriptor{"backgroundChangedRatio", "背景变化面积比", "比例",
                                   AlgorithmDebugMetricGroup::image_detection,
                                   "相对背景变化超过单像素阈值的像素比例。"},
    AlgorithmDebugMetricDescriptor{"roiPixels", "分析像素数", "像素",
                                   AlgorithmDebugMetricGroup::image_detection,
                                   "应用 ROI 和算法降采样后实际参与本次分析的像素数量。"},
    AlgorithmDebugMetricDescriptor{"minimumPaperRatio", "最小纸幅占比", "比例",
                                   AlgorithmDebugMetricGroup::thresholds_background,
                                   "纸幅占比低于该配置阈值时触发纸幅缺失判定。"},
    AlgorithmDebugMetricDescriptor{"maximumMeanGrayscaleChange", "最大灰度变化", "归一化值",
                                   AlgorithmDebugMetricGroup::thresholds_background,
                                   "平均灰度变化达到该配置阈值时触发变化判定。"},
    AlgorithmDebugMetricDescriptor{"maximumBackgroundChange", "最大背景变化", "归一化值",
                                   AlgorithmDebugMetricGroup::thresholds_background,
                                   "背景平均变化达到该配置阈值时触发背景变化判定。"},
    AlgorithmDebugMetricDescriptor{"backgroundPixelChangeThreshold", "背景像素变化阈值", "归一化值",
                                   AlgorithmDebugMetricGroup::thresholds_background,
                                   "单像素相对背景差超过该配置阈值时计入变化面积。"},
    AlgorithmDebugMetricDescriptor{"backgroundLearningRate", "背景学习率", "比例",
                                   AlgorithmDebugMetricGroup::thresholds_background,
                                   "非异常结果用于更新健康背景的单次 EMA 权重。"}};

std::string timestamp_text(const std::chrono::system_clock::time_point value)
{
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count();
    return QDateTime::fromMSecsSinceEpoch(milliseconds, QTimeZone::utc())
        .toString(Qt::ISODateWithMs)
        .toStdString();
}

std::string csv_field(std::string_view value)
{
    std::string escaped{"\""};
    escaped.reserve(value.size() + 2U);
    for (const char character : value)
    {
        if (character == '\"')
            escaped.push_back('\"');
        escaped.push_back(character);
    }
    escaped.push_back('\"');
    return escaped;
}

void append_csv_row(std::string& output, std::span<const std::string> fields)
{
    for (std::size_t index = 0; index < fields.size(); ++index)
    {
        if (index != 0U)
            output.push_back(',');
        output += csv_field(fields[index]);
    }
    output += "\r\n";
}

Error export_error(std::string code, std::string message, const bool retryable = false)
{
    return make_error(std::move(code), Severity::warning, std::move(message), "console",
                      "console.algorithm.exportCurrentValues", retryable);
}

} // namespace

struct AlgorithmMetricHistory::CameraHistory final
{
    std::string camera_id;
    std::uint64_t last_sample_sequence{};
    std::map<std::string, std::deque<AlgorithmMetricPoint>, std::less<>> metrics;
};

AlgorithmMetricHistory::AlgorithmMetricHistory() = default;
AlgorithmMetricHistory::~AlgorithmMetricHistory() = default;

std::span<const AlgorithmMetricDescriptor> algorithm_metric_descriptors() noexcept
{
    return metric_descriptors;
}

std::string_view algorithm_metric_group_name(const AlgorithmMetricGroup group) noexcept
{
    switch (group)
    {
    case AlgorithmMetricGroup::processing_throughput:
        return "处理吞吐";
    case AlgorithmMetricGroup::latency_performance:
        return "时延性能";
    case AlgorithmMetricGroup::backlog_stability:
        return "积压与稳定性";
    case AlgorithmMetricGroup::detection_results:
        return "检测结果";
    }
    return "";
}

std::span<const AlgorithmDebugMetricDescriptor> algorithm_debug_metric_descriptors() noexcept
{
    return debug_descriptors;
}

const AlgorithmDebugMetricDescriptor* find_algorithm_debug_metric(
    const std::string_view key) noexcept
{
    const auto found =
        std::ranges::find(debug_descriptors, key, &AlgorithmDebugMetricDescriptor::key);
    return found == debug_descriptors.end() ? nullptr : &*found;
}

std::string_view algorithm_debug_metric_group_name(const AlgorithmDebugMetricGroup group) noexcept
{
    switch (group)
    {
    case AlgorithmDebugMetricGroup::image_detection:
        return "图像与检测结果";
    case AlgorithmDebugMetricGroup::thresholds_background:
        return "阈值与背景参数";
    case AlgorithmDebugMetricGroup::plugin:
        return "插件指标";
    }
    return "";
}

AlgorithmMetricScalar algorithm_metric_value(const AlgorithmMetricDescriptor& descriptor,
                                             const AlgorithmMetricValue& metrics)
{
    return descriptor.read(metrics);
}

std::string algorithm_metric_value_text(const AlgorithmMetricScalar& value)
{
    return std::visit(
        [](const auto scalar) {
            using Value = decltype(scalar);
            if constexpr (std::is_same_v<Value, bool>)
                return std::string{scalar ? "true" : "false"};
            else if constexpr (std::is_floating_point_v<Value>)
            {
                std::ostringstream stream;
                stream.imbue(std::locale::classic());
                stream << std::setprecision(17) << scalar;
                return stream.str();
            }
            else
                return std::to_string(scalar);
        },
        value);
}

double algorithm_metric_numeric_value(const AlgorithmMetricScalar& value) noexcept
{
    return std::visit([](const auto scalar) { return static_cast<double>(scalar); }, value);
}

bool AlgorithmMetricHistory::ingest(const AlgorithmClientSnapshot& snapshot)
{
    if (snapshot.stale || snapshot.local_sample_sequence == 0U || snapshot.camera_id.empty())
        return false;
    auto camera = std::ranges::find(cameras_, snapshot.camera_id, &CameraHistory::camera_id);
    if (camera == cameras_.end())
    {
        cameras_.push_back({.camera_id = snapshot.camera_id});
        camera = std::prev(cameras_.end());
    }
    if (snapshot.local_sample_sequence <= camera->last_sample_sequence)
        return false;
    camera->last_sample_sequence = snapshot.local_sample_sequence;
    for (const auto& descriptor : metric_descriptors)
    {
        auto& points = camera->metrics[std::string{descriptor.key}];
        points.push_back({.sample_sequence = snapshot.local_sample_sequence,
                          .sample_time = snapshot.local_sample_time,
                          .value = algorithm_metric_numeric_value(
                              algorithm_metric_value(descriptor, snapshot.runtime.metrics))});
        while (points.size() > maximum_points)
            points.pop_front();
    }
    return true;
}

std::vector<AlgorithmMetricPoint> AlgorithmMetricHistory::history(
    const std::string_view camera_id, const std::string_view metric_key) const
{
    const auto camera = std::ranges::find(cameras_, camera_id, &CameraHistory::camera_id);
    if (camera == cameras_.end())
        return {};
    const auto metric = camera->metrics.find(metric_key);
    if (metric == camera->metrics.end())
        return {};
    return {metric->second.begin(), metric->second.end()};
}

std::size_t AlgorithmMetricHistory::size(const std::string_view camera_id,
                                         const std::string_view metric_key) const noexcept
{
    const auto camera = std::ranges::find(cameras_, camera_id, &CameraHistory::camera_id);
    if (camera == cameras_.end())
        return 0U;
    const auto metric = camera->metrics.find(metric_key);
    return metric == camera->metrics.end() ? 0U : metric->second.size();
}

Result<void> export_algorithm_current_values_csv(
    const AlgorithmClientSnapshot& snapshot, const std::filesystem::path& destination,
    const std::chrono::system_clock::time_point exported_at)
{
    if (snapshot.stale || snapshot.local_sample_sequence == 0U)
        return Result<void>::failure(
            export_error("ALGORITHM_SNAPSHOT_STALE", "算法快照已过期，刷新成功后再导出", true));
    if (destination.empty())
        return Result<void>::failure(export_error("FILE_WRITE_FAILED", "导出目标路径不能为空"));

    std::string output{"\xEF\xBB\xBF"};
    const std::array<std::string, 10U> header{"导出时间", "采样时间", "相机", "来源", "分组",
                                              "指标键",   "中文名称", "值",   "单位", "定义"};
    append_csv_row(output, header);
    const std::string exported_text = timestamp_text(exported_at);
    const std::string sampled_text = timestamp_text(snapshot.local_sample_time);
    for (const auto& descriptor : metric_descriptors)
    {
        const std::array<std::string, 10U> row{
            exported_text,
            sampled_text,
            snapshot.camera_id,
            "运行指标",
            std::string{algorithm_metric_group_name(descriptor.group)},
            std::string{descriptor.key},
            std::string{descriptor.chinese_name},
            algorithm_metric_value_text(
                algorithm_metric_value(descriptor, snapshot.runtime.metrics)),
            std::string{descriptor.unit},
            std::string{descriptor.definition}};
        append_csv_row(output, row);
    }
    if (snapshot.test_result)
    {
        for (const auto& metric : snapshot.test_result->debug_metrics)
        {
            const auto* descriptor = find_algorithm_debug_metric(metric.name);
            const std::string chinese_name =
                descriptor ? std::string{descriptor->chinese_name} : metric.name;
            const std::string unit = descriptor ? std::string{descriptor->unit} : "插件值";
            const std::string group =
                descriptor ? std::string{algorithm_debug_metric_group_name(descriptor->group)}
                           : std::string{algorithm_debug_metric_group_name(
                                 AlgorithmDebugMetricGroup::plugin)};
            const std::string definition =
                descriptor ? std::string{descriptor->definition}
                           : "检测器插件返回的单帧调试值；口径、范围和重置语义由该插件定义。";
            const std::array<std::string, 10U> row{
                exported_text,
                sampled_text,
                snapshot.camera_id,
                "调试指标",
                group,
                metric.name,
                chinese_name,
                algorithm_metric_value_text(AlgorithmMetricScalar{metric.value}),
                unit,
                definition};
            append_csv_row(output, row);
        }
    }

    QSaveFile file{QString::fromStdWString(destination.wstring())};
    if (!file.open(QIODevice::WriteOnly))
        return Result<void>::failure(export_error("FILE_WRITE_FAILED", "无法创建算法指标 CSV"));
    const auto written = file.write(output.data(), static_cast<qint64>(output.size()));
    if (written != static_cast<qint64>(output.size()) || !file.commit())
        return Result<void>::failure(export_error("FILE_WRITE_FAILED", "无法原子保存算法指标 CSV"));
    return Result<void>::success();
}

} // namespace paperbreak::console
