#include "paperbreak/camera/acquisition.hpp"
#include "paperbreak/camera/hikrobot_camera.hpp"

#include <Windows.h>

#include <Iphlpapi.h>
#include <Psapi.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
using Json = nlohmann::json;
using namespace std::chrono_literals;
using paperbreak::camera::AcquisitionQueue;
using paperbreak::camera::AcquisitionWorker;
using paperbreak::camera::AcquisitionWorkerOptions;
using paperbreak::camera::CameraParameterSnapshot;
using paperbreak::camera::CameraSlotBinding;
using paperbreak::camera::FrameBufferPool;
using paperbreak::camera::FrameDequeueStatus;
using paperbreak::camera::ICameraDevice;
using paperbreak::camera::ICameraProvider;
using paperbreak::camera::TriggerMode;

constexpr std::size_t maximum_cameras = 4U;
constexpr std::size_t maximum_capacity = 256U;
constexpr std::uint32_t maximum_duration_seconds = 3600U;
constexpr std::size_t maximum_samples = 3601U;
constexpr std::size_t maximum_frame_buffer_bytes = 64U * 1024U * 1024U;
constexpr std::size_t maximum_pool_bytes_per_camera = 512U * 1024U * 1024U;

struct Binding final
{
    std::string camera_id;
    std::string serial_number;
};

struct Plan final
{
    std::string target_model;
    std::vector<Binding> bindings;
    std::uint32_t duration_seconds{};
    std::uint32_t sample_interval_ms{};
    std::uint32_t receive_timeout_ms{};
    std::size_t queue_capacity{};
    std::size_t pool_capacity{};
    std::size_t frame_buffer_bytes{};
    std::uint32_t consumer_delay_ms{};
    double minimum_fps_ratio{};
    CameraParameterSnapshot parameters;
};

std::string utc_now()
{
    SYSTEMTIME time{};
    GetSystemTime(&time);
    char buffer[32]{};
    static_cast<void>(std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
                                    time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
                                    time.wSecond, time.wMilliseconds));
    return buffer;
}

Json error_json(const paperbreak::Error& error)
{
    Json details = Json::array();
    for (const auto& detail : error.details)
        details.push_back({{"key", detail.key}, {"value", detail.value}});
    return {{"businessCode", error.business_code},
            {"module", error.module},
            {"operation", error.operation},
            {"message", error.message},
            {"nativeDomain", error.native_domain.value_or("")},
            {"nativeCode", error.native_code.value_or("")},
            {"details", std::move(details)}};
}

std::optional<TriggerMode> parse_trigger_mode(const std::string_view value)
{
    if (value == "continuous")
        return TriggerMode::continuous;
    if (value == "software")
        return TriggerMode::software;
    if (value == "hardware")
        return TriggerMode::hardware;
    return std::nullopt;
}

paperbreak::Result<Plan> load_plan(const std::filesystem::path& path)
{
    auto invalid = [](std::string message) {
        return paperbreak::Result<Plan>::failure(paperbreak::make_error(
            "HW_PLAN_INVALID", paperbreak::Severity::error, std::move(message), "hardware-test",
            "hardwareTest.loadPlan", false));
    };
    try
    {
        std::ifstream stream{path};
        if (!stream)
            return invalid("无法打开硬件测试计划");
        Json root;
        stream >> root;
        if (root.value("schemaVersion", 0) != 1)
            return invalid("硬件测试计划 schemaVersion 必须为 1");
        Plan plan;
        plan.target_model = root.value("targetModel", "");
        const auto& capture = root.at("capture");
        plan.duration_seconds = capture.value("durationSeconds", 0U);
        plan.sample_interval_ms = capture.value("sampleIntervalMs", 0U);
        plan.receive_timeout_ms = capture.value("receiveTimeoutMs", 0U);
        plan.queue_capacity = capture.value("queueCapacity", 0U);
        plan.pool_capacity = capture.value("framePoolCapacity", 0U);
        plan.frame_buffer_bytes = capture.value("frameBufferBytes", 0U);
        plan.consumer_delay_ms = capture.value("consumerDelayMs", 0U);
        plan.minimum_fps_ratio = capture.value("minimumFpsRatio", 0.0);
        for (const auto& item : root.at("bindings"))
            plan.bindings.push_back({.camera_id = item.value("cameraId", ""),
                                     .serial_number = item.value("serialNumber", "")});
        const auto& parameters = root.at("parameters");
        if (parameters.contains("exposureUs"))
            plan.parameters.exposure_us = parameters.at("exposureUs").get<double>();
        if (parameters.contains("gainDb"))
            plan.parameters.gain_db = parameters.at("gainDb").get<double>();
        if (parameters.contains("frameRate"))
            plan.parameters.frame_rate = parameters.at("frameRate").get<double>();
        if (parameters.contains("packetSizeBytes"))
            plan.parameters.packet_size_bytes =
                parameters.at("packetSizeBytes").get<std::uint32_t>();
        if (parameters.contains("interPacketDelay"))
            plan.parameters.inter_packet_delay_ns =
                parameters.at("interPacketDelay").get<std::uint32_t>();
        if (parameters.contains("triggerMode"))
        {
            plan.parameters.trigger_mode =
                parse_trigger_mode(parameters.at("triggerMode").get<std::string>());
            if (!plan.parameters.trigger_mode)
                return invalid("triggerMode 必须为 continuous、software 或 hardware");
        }
        if (parameters.contains("triggerSource"))
            plan.parameters.trigger_source = parameters.at("triggerSource").get<std::string>();

        if (plan.target_model.empty() || plan.bindings.empty() ||
            plan.bindings.size() > maximum_cameras || plan.duration_seconds == 0U ||
            plan.duration_seconds > maximum_duration_seconds || plan.sample_interval_ms < 100U ||
            plan.sample_interval_ms > 2000U || plan.receive_timeout_ms == 0U ||
            plan.receive_timeout_ms > 10000U || plan.consumer_delay_ms > 1000U ||
            plan.queue_capacity == 0U || plan.queue_capacity > maximum_capacity ||
            plan.pool_capacity < 2U || plan.pool_capacity > maximum_capacity ||
            plan.frame_buffer_bytes == 0U || plan.frame_buffer_bytes > maximum_frame_buffer_bytes ||
            plan.pool_capacity > maximum_pool_bytes_per_camera / plan.frame_buffer_bytes ||
            plan.minimum_fps_ratio <= 0.0 || plan.minimum_fps_ratio > 1.0)
            return invalid("硬件测试计划字段缺失或超出有界范围");
        const auto sample_count =
            (static_cast<std::size_t>(plan.duration_seconds) * 1000U) / plan.sample_interval_ms +
            2U;
        if (sample_count > maximum_samples)
            return invalid("资源采样数超过 3601 的硬上限");
        std::unordered_set<std::string> ids;
        std::unordered_set<std::string> serials;
        for (std::size_t index = 0; index < plan.bindings.size(); ++index)
        {
            const std::string expected = "CAM0" + std::to_string(index + 1U);
            if (plan.bindings[index].camera_id != expected ||
                plan.bindings[index].serial_number.empty() ||
                !ids.insert(plan.bindings[index].camera_id).second ||
                !serials.insert(plan.bindings[index].serial_number).second)
                return invalid("bindings 必须从 CAM01 连续排列且逻辑 ID/序列号唯一");
        }
        return paperbreak::Result<Plan>::success(std::move(plan));
    }
    catch (const std::exception& exception)
    {
        return invalid(std::string{"硬件测试计划解析失败："} + exception.what());
    }
}

std::uint64_t file_time_value(const FILETIME& value)
{
    ULARGE_INTEGER converted{};
    converted.LowPart = value.dwLowDateTime;
    converted.HighPart = value.dwHighDateTime;
    return converted.QuadPart;
}

struct ResourceBaseline final
{
    std::chrono::steady_clock::time_point wall;
    std::uint64_t process_ticks{};
    std::uint64_t network_bytes{};
};

std::uint64_t network_bytes()
{
    ULONG size{};
    static_cast<void>(GetIfTable(nullptr, &size, FALSE));
    constexpr ULONG maximum_table_bytes = 1024U * 1024U;
    if (size == 0U || size > maximum_table_bytes)
        return 0U;
    std::vector<std::byte> storage(size);
    auto* table = reinterpret_cast<MIB_IFTABLE*>(storage.data());
    if (GetIfTable(table, &size, FALSE) != NO_ERROR)
        return 0U;
    std::uint64_t total{};
    for (DWORD index = 0U; index < table->dwNumEntries; ++index)
    {
        const auto& entry = table->table[index];
        if (entry.dwOperStatus == IF_OPER_STATUS_OPERATIONAL &&
            entry.dwType != MIB_IF_TYPE_LOOPBACK)
            total += static_cast<std::uint64_t>(entry.dwInOctets) + entry.dwOutOctets;
    }
    return total;
}

Json resource_sample(std::optional<ResourceBaseline>& previous)
{
    FILETIME created{}, exited{}, kernel{}, user{};
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    const bool times_ok = GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    const bool memory_ok = GetProcessMemoryInfo(
        GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
    const auto now = std::chrono::steady_clock::now();
    const std::uint64_t ticks = times_ok ? file_time_value(kernel) + file_time_value(user) : 0U;
    const std::uint64_t bytes = network_bytes();
    double cpu_percent{};
    double network_bps{};
    bool rate_available = false;
    if (previous)
    {
        const double seconds = std::chrono::duration<double>(now - previous->wall).count();
        if (seconds > 0.0 && ticks >= previous->process_ticks && bytes >= previous->network_bytes)
        {
            const auto processors =
                std::max<DWORD>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS), 1U);
            cpu_percent = static_cast<double>(ticks - previous->process_ticks) / 10'000'000.0 /
                          seconds * 100.0 / static_cast<double>(processors);
            network_bps = static_cast<double>(bytes - previous->network_bytes) / seconds;
            rate_available = true;
        }
    }
    previous = ResourceBaseline{now, ticks, bytes};
    return {{"utc", utc_now()},
            {"cpuPercent", cpu_percent},
            {"workingSetBytes", memory_ok ? memory.WorkingSetSize : 0U},
            {"networkBytesPerSecond", network_bps},
            {"ratesAvailable", rate_available}};
}

Json inventory_json(const std::vector<paperbreak::camera::CameraDeviceDescriptor>& devices)
{
    Json result = Json::array();
    for (const auto& device : devices)
        result.push_back({{"model", device.model_name},
                          {"serialNumber", device.serial_number},
                          {"ipAddress", device.ip_address},
                          {"networkInterface", device.network_interface},
                          {"exclusiveAccessAvailable", device.exclusive_access_available}});
    return result;
}

std::string trigger_mode_text(const TriggerMode mode)
{
    switch (mode)
    {
    case TriggerMode::continuous:
        return "continuous";
    case TriggerMode::software:
        return "software";
    case TriggerMode::hardware:
        return "hardware";
    }
    return "unknown";
}

Json parameters_json(const CameraParameterSnapshot& value)
{
    Json result = Json::object();
    if (value.exposure_us)
        result["exposureUs"] = *value.exposure_us;
    if (value.gain_db)
        result["gainDb"] = *value.gain_db;
    if (value.frame_rate)
        result["frameRate"] = *value.frame_rate;
    if (value.trigger_mode)
        result["triggerMode"] = trigger_mode_text(*value.trigger_mode);
    if (value.trigger_source)
        result["triggerSource"] = *value.trigger_source;
    if (value.packet_size_bytes)
        result["packetSizeBytes"] = *value.packet_size_bytes;
    if (value.inter_packet_delay_ns)
        result["interPacketDelay"] = *value.inter_packet_delay_ns;
    if (value.pixel_format)
        result["pixelFormat"] = static_cast<int>(*value.pixel_format);
    if (value.roi)
        result["roi"] = {{"width", value.roi->width},
                         {"height", value.roi->height},
                         {"offsetX", value.roi->offset_x},
                         {"offsetY", value.roi->offset_y}};
    return result;
}

paperbreak::Result<void> write_record(const std::filesystem::path& output, const Json& record)
{
    auto failure = [](std::string message) {
        return paperbreak::Result<void>::failure(paperbreak::make_error(
            "HW_RECORD_WRITE_FAILED", paperbreak::Severity::error, std::move(message),
            "hardware-test", "hardwareTest.writeRecord", false));
    };
    if (std::filesystem::exists(output))
        return failure("审计记录已存在，拒绝覆盖");
    const auto temporary = output.string() + ".tmp";
    std::error_code error;
    if (!output.parent_path().empty())
    {
        std::filesystem::create_directories(output.parent_path(), error);
        if (error)
            return failure("无法创建审计记录目录");
    }
    {
        std::ofstream stream{temporary, std::ios::binary | std::ios::trunc};
        if (!stream)
            return failure("无法创建审计记录临时文件");
        stream << record.dump(2) << '\n';
        if (!stream)
            return failure("无法完整写入审计记录临时文件");
    }
    std::filesystem::rename(temporary, output, error);
    if (error)
        return failure("无法原子提交审计记录");
    return paperbreak::Result<void>::success();
}

Json run_stage(ICameraProvider& provider, const Plan& plan, const std::size_t camera_count)
{
    Json stage{{"cameraCount", camera_count}, {"startedUtc", utc_now()}, {"status", "failed"}};
    std::vector<std::unique_ptr<ICameraDevice>> devices;
    std::vector<std::unique_ptr<FrameBufferPool>> pools;
    std::vector<std::unique_ptr<AcquisitionQueue>> queues;
    std::vector<std::unique_ptr<AcquisitionWorker>> workers;
    std::vector<CameraParameterSnapshot> actual_parameters;
    std::vector<std::jthread> consumers;
    std::atomic<std::uint64_t> consumed{};
    auto cleanup = [&] {
        for (auto& consumer : consumers)
            consumer.request_stop();
        for (auto& worker : workers)
            worker->request_stop();
        for (auto& queue : queues)
            queue->close();
        for (auto& worker : workers)
            static_cast<void>(worker->join(std::chrono::steady_clock::now() + 5s));
        consumers.clear();
        for (auto& device : devices)
        {
            static_cast<void>(device->stop_acquisition());
            static_cast<void>(device->disconnect());
        }
    };

    for (std::size_t index = 0U; index < camera_count; ++index)
    {
        auto created = provider.create_device(plan.bindings[index].serial_number);
        if (!created)
        {
            stage["error"] = error_json(created.error());
            cleanup();
            return stage;
        }
        auto device = std::move(created).value();
        if (auto connected = device->connect(); !connected)
        {
            stage["error"] = error_json(connected.error());
            cleanup();
            return stage;
        }
        auto applied = device->apply_parameters(plan.parameters);
        if (!applied)
        {
            stage["error"] = error_json(applied.error());
            static_cast<void>(device->disconnect());
            cleanup();
            return stage;
        }
        actual_parameters.push_back(applied.value());
        if (auto started = device->start_acquisition(); !started)
        {
            stage["error"] = error_json(started.error());
            static_cast<void>(device->disconnect());
            cleanup();
            return stage;
        }
        devices.push_back(std::move(device));
        pools.push_back(
            std::make_unique<FrameBufferPool>(plan.pool_capacity, plan.frame_buffer_bytes));
        queues.push_back(std::make_unique<AcquisitionQueue>(plan.queue_capacity));
        workers.push_back(std::make_unique<AcquisitionWorker>(
            *devices.back(), *pools.back(), *queues.back(),
            AcquisitionWorkerOptions{
                .camera_id = plan.bindings[index].camera_id,
                .receive_timeout = std::chrono::milliseconds{plan.receive_timeout_ms},
                .statistics_window = 1s,
                .consecutive_timeout_limit = 3U,
                .software_trigger_interval =
                    plan.parameters.trigger_mode == TriggerMode::software
                        ? std::optional<std::chrono::milliseconds>{plan.sample_interval_ms}
                        : std::nullopt}));
        if (auto started = workers.back()->start(); !started)
        {
            stage["error"] = error_json(started.error());
            cleanup();
            return stage;
        }
        auto* queue = queues.back().get();
        consumers.emplace_back([&, queue](std::stop_token token) {
            while (!token.stop_requested())
            {
                auto item = queue->wait_pop(token, 100ms);
                if (item.status == FrameDequeueStatus::frame)
                {
                    consumed.fetch_add(1U, std::memory_order_relaxed);
                    if (plan.consumer_delay_ms > 0U)
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds{plan.consumer_delay_ms});
                }
                else if (item.status == FrameDequeueStatus::closed ||
                         item.status == FrameDequeueStatus::stopped)
                    break;
            }
        });
    }

    std::optional<ResourceBaseline> baseline;
    Json samples = Json::array();
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{plan.duration_seconds};
    while (std::chrono::steady_clock::now() < deadline)
    {
        samples.push_back(resource_sample(baseline));
        std::this_thread::sleep_for(std::chrono::milliseconds{plan.sample_interval_ms});
    }
    cleanup();

    Json cameras = Json::array();
    bool passed = !stage.contains("triggerErrors");
    for (std::size_t index = 0U; index < camera_count; ++index)
    {
        const auto acquisition = workers[index]->snapshot();
        const auto queue = queues[index]->snapshot();
        const auto pool = pools[index]->snapshot();
        const double target_fps = plan.parameters.frame_rate.value_or(0.0);
        const bool rate_passed =
            target_fps <= 0.0 || acquisition.actual_fps >= target_fps * plan.minimum_fps_ratio;
        passed = passed && rate_passed && !acquisition.last_error.has_value();
        Json camera{{"cameraId", plan.bindings[index].camera_id},
                    {"serialNumber", plan.bindings[index].serial_number},
                    {"actualFps", acquisition.actual_fps},
                    {"framesReceived", acquisition.frames_received},
                    {"cameraFrameGaps", acquisition.camera_frame_gaps},
                    {"captureTimeouts", acquisition.capture_timeouts},
                    {"incompleteFrames", acquisition.incomplete_frames},
                    {"bandwidthBytesPerSecond", acquisition.bandwidth_bytes_per_second},
                    {"queueCapacity", queue.capacity},
                    {"queueHighWatermark", queue.high_watermark},
                    {"queueDroppedOldest", queue.dropped_oldest},
                    {"poolCapacity", pool.capacity},
                    {"poolExhausted", pool.exhausted},
                    {"actualParameters", parameters_json(actual_parameters[index])},
                    {"frameRateThresholdPassed", rate_passed}};
        if (acquisition.last_error)
            camera["error"] = error_json(*acquisition.last_error);
        cameras.push_back(std::move(camera));
    }
    stage["status"] = passed ? "passed" : "failed";
    stage["finishedUtc"] = utc_now();
    stage["consumedFrames"] = consumed.load(std::memory_order_relaxed);
    stage["cameras"] = std::move(cameras);
    stage["resourceSamples"] = std::move(samples);
    return stage;
}

int print_error(const paperbreak::Error& error, const int exit_code)
{
    std::cerr << error.business_code << ": " << error.message << '\n';
    return exit_code;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::string_view{argv[1]} == "--help")
    {
        std::cout
            << "PaperBreakCameraHardwareTest --probe --output <record.json>\n"
               "PaperBreakCameraHardwareTest --run --plan <plan.json> --output <record.json>\n";
        return 0;
    }
    bool probe = false;
    bool run = false;
    std::filesystem::path plan_path;
    std::filesystem::path output_path;
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument{argv[index]};
        if (argument == "--probe")
            probe = true;
        else if (argument == "--run")
            run = true;
        else if (argument == "--plan" && index + 1 < argc)
            plan_path = argv[++index];
        else if (argument == "--output" && index + 1 < argc)
            output_path = argv[++index];
        else
        {
            std::cerr << "HW_PLAN_INVALID: 未知或缺少命令行参数\n";
            return 2;
        }
    }
    if (probe == run || output_path.empty() || (run && plan_path.empty()))
    {
        std::cerr << "HW_PLAN_INVALID: 必须选择 --probe 或 --run，并提供所需路径\n";
        return 2;
    }

    std::optional<Plan> approved_plan;
    if (run)
    {
        auto loaded = load_plan(plan_path);
        if (!loaded)
            return print_error(loaded.error(), 2);
        approved_plan.emplace(std::move(loaded).value());
    }

    auto provider = paperbreak::camera::hikrobot::create_hikrobot_camera_provider();
    auto enumerated = provider->enumerate_devices();
    Json record{{"schemaVersion", 1},
                {"tool", "PaperBreakCameraHardwareTest"},
                {"startedUtc", utc_now()},
                {"mode", probe ? "probe" : "run"},
                {"hardwareGate", "incomplete"},
                {"manualScenarios",
                 {{{"id", "camera-cable-disconnect"}, {"status", "not-executed"}},
                  {{"id", "switch-link-interruption"}, {"status", "not-executed"}},
                  {{"id", "service-restart"}, {"status", "not-executed"}},
                  {{"id", "real-preview-log-isolation"}, {"status", "not-executed"}}}}};
    if (!enumerated)
    {
        record["inventoryError"] = error_json(enumerated.error());
        record["finishedUtc"] = utc_now();
        if (const auto written = write_record(output_path, record); !written)
            return print_error(written.error(), 3);
        return print_error(enumerated.error(), 4);
    }
    record["inventory"] = inventory_json(enumerated.value());
    if (probe)
    {
        record["finishedUtc"] = utc_now();
        if (const auto written = write_record(output_path, record); !written)
            return print_error(written.error(), 3);
        std::cout << output_path.string() << '\n';
        return enumerated.value().empty() ? 4 : 0;
    }

    std::vector<CameraSlotBinding> bindings;
    const Plan& plan = *approved_plan;
    record["approvedPlan"] = {{"targetModel", plan.target_model},
                              {"requestedParameters", parameters_json(plan.parameters)},
                              {"durationSeconds", plan.duration_seconds},
                              {"sampleIntervalMs", plan.sample_interval_ms},
                              {"receiveTimeoutMs", plan.receive_timeout_ms},
                              {"queueCapacity", plan.queue_capacity},
                              {"framePoolCapacity", plan.pool_capacity},
                              {"frameBufferBytes", plan.frame_buffer_bytes},
                              {"consumerDelayMs", plan.consumer_delay_ms},
                              {"minimumFpsRatio", plan.minimum_fps_ratio}};
    for (const auto& binding : plan.bindings)
        bindings.push_back({binding.camera_id, binding.serial_number});
    auto reconciliation = paperbreak::camera::reconcile_camera_slots(bindings, enumerated.value());
    bool execution_passed = false;
    if (!reconciliation)
    {
        record["bindingError"] = error_json(reconciliation.error());
    }
    else
    {
        bool ready = true;
        record["bindings"] = Json::array();
        for (const auto& slot : reconciliation.value().slots)
        {
            ready = ready && slot.status == paperbreak::camera::CameraSlotStatus::ready &&
                    slot.device && slot.device->model_name == plan.target_model;
            record["bindings"].push_back(
                {{"cameraId", slot.camera_id},
                 {"serialNumber", slot.serial_number},
                 {"status", static_cast<int>(slot.status)},
                 {"actualModel", slot.device ? slot.device->model_name : ""}});
        }
        record["bindingReady"] = ready;
        if (ready)
        {
            record["stages"] = Json::array();
            execution_passed = true;
            for (std::size_t count = 1U; count <= plan.bindings.size(); ++count)
            {
                auto stage = run_stage(*provider, plan, count);
                execution_passed = execution_passed && stage.value("status", "failed") == "passed";
                record["stages"].push_back(std::move(stage));
            }
        }
    }
    record["executionPassed"] = execution_passed;
    record["hardwareGate"] = execution_passed ? "functional-evidence-passed" : "incomplete";
    record["finishedUtc"] = utc_now();
    if (const auto written = write_record(output_path, record); !written)
        return print_error(written.error(), 3);
    std::cout << output_path.string() << '\n';
    return execution_passed ? 0 : 5;
}
