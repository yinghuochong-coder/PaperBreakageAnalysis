#include "paperbreak/algorithm/detector_host.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace paperbreak::algorithm
{
namespace
{

constexpr std::size_t maximum_parameters = 64U;
constexpr std::size_t maximum_identifier_length = 128U;
constexpr std::size_t maximum_parameter_name_length = 128U;
constexpr std::size_t maximum_string_parameter_length = 4096U;
constexpr auto maximum_processing_timeout = std::chrono::seconds{60};

Error host_error(std::string code, std::string message, std::string operation,
                 const std::string_view source_id = {})
{
    auto error = make_error(std::move(code), Severity::error, std::move(message), "algorithm",
                            std::move(operation));
    if (!source_id.empty())
        error.source_id = std::string{source_id};
    return error;
}

Error exception_error(const std::string_view operation, const std::string_view source_id,
                      const std::string_view exception_type, const std::string_view what = {})
{
    auto error = host_error("ALGORITHM_PLUGIN_EXCEPTION", "检测器插件发生未处理异常",
                            std::string{operation}, source_id);
    error.details.push_back({"exceptionType", std::string{exception_type}});
    if (!what.empty())
        error.details.push_back({"what", std::string{what}});
    return error;
}

void add_stage(Error& error, const std::string_view stage)
{
    error.details.push_back({"hostStage", std::string{stage}});
}

Result<void> validate_config(const DetectorConfig& config)
{
    if (config.plugin_id.empty() || config.plugin_id.size() > maximum_identifier_length)
    {
        return Result<void>::failure(host_error("SYS_CONFIG_INVALID", "检测器插件标识无效",
                                                "algorithm.detector.validateConfig",
                                                config.plugin_id));
    }
    if (config.camera_id.empty() || config.camera_id.size() > maximum_identifier_length)
    {
        return Result<void>::failure(host_error("SYS_CONFIG_INVALID", "检测器相机标识无效",
                                                "algorithm.detector.validateConfig",
                                                config.camera_id));
    }
    if (config.revision == 0U || config.processing_timeout.count() <= 0 ||
        config.processing_timeout > maximum_processing_timeout ||
        config.parameters.size() > maximum_parameters)
    {
        return Result<void>::failure(host_error("SYS_CONFIG_INVALID", "检测器配置边界无效",
                                                "algorithm.detector.validateConfig",
                                                config.camera_id));
    }

    std::vector<std::string_view> names;
    names.reserve(config.parameters.size());
    for (const auto& parameter : config.parameters)
    {
        if (parameter.name.empty() || parameter.name.size() > maximum_parameter_name_length ||
            std::ranges::find(names, parameter.name) != names.end())
        {
            return Result<void>::failure(host_error("SYS_CONFIG_INVALID", "检测器参数名称无效",
                                                    "algorithm.detector.validateConfig",
                                                    config.camera_id));
        }
        if (const auto* value = std::get_if<double>(&parameter.value);
            value != nullptr && !std::isfinite(*value))
        {
            return Result<void>::failure(host_error("SYS_CONFIG_INVALID", "检测器参数数值无效",
                                                    "algorithm.detector.validateConfig",
                                                    config.camera_id));
        }
        if (const auto* value = std::get_if<std::string>(&parameter.value);
            value != nullptr && value->size() > maximum_string_parameter_length)
        {
            return Result<void>::failure(host_error("SYS_CONFIG_INVALID", "检测器字符串参数过长",
                                                    "algorithm.detector.validateConfig",
                                                    config.camera_id));
        }
        names.push_back(parameter.name);
    }
    return Result<void>::success();
}

Result<void> invoke_initialize(IBreakDetector& detector, const DetectorConfig& config,
                               const std::string_view stage)
{
    try
    {
        auto result = detector.initialize(config);
        if (!result)
        {
            auto error = std::move(result.error());
            add_stage(error, stage);
            return Result<void>::failure(std::move(error));
        }
        return Result<void>::success();
    }
    catch (const std::exception& exception)
    {
        return Result<void>::failure(exception_error(
            "algorithm.detector.initialize", config.camera_id, "std::exception", exception.what()));
    }
    catch (...)
    {
        return Result<void>::failure(
            exception_error("algorithm.detector.initialize", config.camera_id, "unknown"));
    }
}

Result<void> invoke_update(IBreakDetector& detector, const DetectorConfig& config)
{
    try
    {
        auto result = detector.update_config(config);
        if (!result)
        {
            auto error = std::move(result.error());
            add_stage(error, "update-config");
            return Result<void>::failure(std::move(error));
        }
        return Result<void>::success();
    }
    catch (const std::exception& exception)
    {
        return Result<void>::failure(exception_error("algorithm.detector.updateConfig",
                                                     config.camera_id, "std::exception",
                                                     exception.what()));
    }
    catch (...)
    {
        return Result<void>::failure(
            exception_error("algorithm.detector.updateConfig", config.camera_id, "unknown"));
    }
}

Result<DetectorInfo> invoke_info(const IBreakDetector& detector, const std::string_view source_id)
{
    try
    {
        auto info = detector.info();
        if (info.plugin_id.empty() || info.implementation_version.empty())
        {
            return Result<DetectorInfo>::failure(host_error("ALGORITHM_PLUGIN_LOAD_FAILED",
                                                            "检测器插件信息不完整",
                                                            "algorithm.detector.info", source_id));
        }
        return Result<DetectorInfo>::success(std::move(info));
    }
    catch (const std::exception& exception)
    {
        return Result<DetectorInfo>::failure(exception_error("algorithm.detector.info", source_id,
                                                             "std::exception", exception.what()));
    }
    catch (...)
    {
        return Result<DetectorInfo>::failure(
            exception_error("algorithm.detector.info", source_id, "unknown"));
    }
}

} // namespace

struct DetectorPluginRegistry::Impl final
{
    struct Entry final
    {
        std::string plugin_id;
        DetectorFactory factory;
    };

    std::vector<Entry> entries;
};

DetectorPluginRegistry::DetectorPluginRegistry() : impl_(std::make_unique<Impl>())
{
    impl_->entries.reserve(maximum_plugins);
}

DetectorPluginRegistry::~DetectorPluginRegistry() = default;

Result<void> DetectorPluginRegistry::register_plugin(std::string plugin_id, DetectorFactory factory)
{
    if (plugin_id.empty() || plugin_id.size() > maximum_identifier_length || !factory)
    {
        return Result<void>::failure(host_error("ALGORITHM_PLUGIN_LOAD_FAILED",
                                                "检测器插件注册参数无效",
                                                "algorithm.detector.register", plugin_id));
    }
    if (impl_->entries.size() >= maximum_plugins)
    {
        return Result<void>::failure(host_error("ALGORITHM_PLUGIN_LOAD_FAILED",
                                                "检测器插件注册表已满",
                                                "algorithm.detector.register", plugin_id));
    }
    if (std::ranges::find(impl_->entries, plugin_id, &Impl::Entry::plugin_id) !=
        impl_->entries.end())
    {
        return Result<void>::failure(host_error("ALGORITHM_PLUGIN_LOAD_FAILED",
                                                "检测器插件标识重复", "algorithm.detector.register",
                                                plugin_id));
    }
    impl_->entries.push_back({.plugin_id = std::move(plugin_id), .factory = std::move(factory)});
    return Result<void>::success();
}

Result<std::unique_ptr<IBreakDetector>> DetectorPluginRegistry::create(
    const std::string_view plugin_id) const
{
    const auto found = std::ranges::find(impl_->entries, plugin_id, &Impl::Entry::plugin_id);
    if (found == impl_->entries.end())
    {
        return Result<std::unique_ptr<IBreakDetector>>::failure(
            host_error("ALGORITHM_PLUGIN_LOAD_FAILED", "未找到已编译的检测器插件",
                       "algorithm.detector.create", plugin_id));
    }
    try
    {
        auto result = found->factory();
        if (!result)
        {
            auto error = std::move(result.error());
            add_stage(error, "factory");
            return Result<std::unique_ptr<IBreakDetector>>::failure(std::move(error));
        }
        auto detector = std::move(result).value();
        if (!detector)
        {
            return Result<std::unique_ptr<IBreakDetector>>::failure(
                host_error("ALGORITHM_PLUGIN_LOAD_FAILED", "检测器插件工厂返回空实例",
                           "algorithm.detector.create", plugin_id));
        }
        return Result<std::unique_ptr<IBreakDetector>>::success(std::move(detector));
    }
    catch (const std::exception& exception)
    {
        return Result<std::unique_ptr<IBreakDetector>>::failure(exception_error(
            "algorithm.detector.create", plugin_id, "std::exception", exception.what()));
    }
    catch (...)
    {
        return Result<std::unique_ptr<IBreakDetector>>::failure(
            exception_error("algorithm.detector.create", plugin_id, "unknown"));
    }
}

std::size_t DetectorPluginRegistry::size() const noexcept
{
    return impl_->entries.size();
}

struct DetectorHost::Impl final
{
    explicit Impl(const DetectorPluginRegistry& value) : registry(&value) {}

    const DetectorPluginRegistry* registry;
    std::unique_ptr<IBreakDetector> active;
    std::optional<DetectorConfig> config;
    std::optional<DetectorInfo> active_info;
    DetectorHostMetrics metrics;
};

DetectorHost::DetectorHost(const DetectorPluginRegistry& registry)
    : impl_(std::make_unique<Impl>(registry))
{
}

DetectorHost::~DetectorHost() = default;

Result<void> DetectorHost::load(const DetectorConfig& config)
{
    if (auto valid = validate_config(config); !valid)
        return valid;

    auto created = impl_->registry->create(config.plugin_id);
    if (!created)
        return Result<void>::failure(std::move(created.error()));
    auto candidate = std::move(created).value();
    if (auto initialized = invoke_initialize(*candidate, config, "load-initialize"); !initialized)
        return initialized;
    auto plugin_info = invoke_info(*candidate, config.camera_id);
    if (!plugin_info)
        return Result<void>::failure(std::move(plugin_info.error()));
    if (plugin_info.value().plugin_id != config.plugin_id)
    {
        return Result<void>::failure(host_error("ALGORITHM_PLUGIN_LOAD_FAILED",
                                                "检测器插件标识与注册项不一致",
                                                "algorithm.detector.load", config.plugin_id));
    }

    impl_->active = std::move(candidate);
    impl_->config = config;
    impl_->active_info = std::move(plugin_info).value();
    return Result<void>::success();
}

Result<void> DetectorHost::update_config(const DetectorConfig& config)
{
    if (!impl_->active || !impl_->config)
    {
        return Result<void>::failure(host_error("ALGORITHM_NOT_READY", "检测器尚未装载",
                                                "algorithm.detector.updateConfig",
                                                config.camera_id));
    }
    if (auto valid = validate_config(config); !valid)
        return valid;
    if (config.plugin_id != impl_->config->plugin_id ||
        config.camera_id != impl_->config->camera_id || config.revision <= impl_->config->revision)
    {
        return Result<void>::failure(
            host_error("SYS_CONFIG_INVALID", "检测器热更新身份或修订号无效",
                       "algorithm.detector.updateConfig", config.camera_id));
    }

    auto created = impl_->registry->create(config.plugin_id);
    if (!created)
        return Result<void>::failure(std::move(created.error()));
    auto candidate = std::move(created).value();
    if (auto initialized =
            invoke_initialize(*candidate, *impl_->config, "rollback-baseline-initialize");
        !initialized)
    {
        return initialized;
    }
    if (auto updated = invoke_update(*candidate, config); !updated)
        return updated;
    auto plugin_info = invoke_info(*candidate, config.camera_id);
    if (!plugin_info)
        return Result<void>::failure(std::move(plugin_info.error()));
    if (plugin_info.value().plugin_id != config.plugin_id)
    {
        return Result<void>::failure(
            host_error("ALGORITHM_PLUGIN_LOAD_FAILED", "更新后的检测器插件标识不一致",
                       "algorithm.detector.updateConfig", config.plugin_id));
    }

    impl_->active = std::move(candidate);
    impl_->config = config;
    impl_->active_info = std::move(plugin_info).value();
    ++impl_->metrics.successful_config_updates;
    return Result<void>::success();
}

Result<DetectionResult> DetectorHost::process(const camera::FrameView& frame)
{
    if (!impl_->active || !impl_->config)
    {
        return Result<DetectionResult>::failure(host_error("ALGORITHM_NOT_READY", "检测器尚未装载",
                                                           "algorithm.detector.process",
                                                           frame.camera_id()));
    }
    if (frame.camera_id() != impl_->config->camera_id)
    {
        return Result<DetectionResult>::failure(
            host_error("ALGORITHM_PROCESS_FAILED", "帧与检测器绑定相机不一致",
                       "algorithm.detector.process", frame.camera_id()));
    }

    ++impl_->metrics.process_calls;
    const auto started = std::chrono::steady_clock::now();
    std::optional<Result<DetectionResult>> result;
    try
    {
        result.emplace(impl_->active->process(frame));
    }
    catch (const std::exception& exception)
    {
        result.emplace(Result<DetectionResult>::failure(exception_error(
            "algorithm.detector.process", frame.camera_id(), "std::exception", exception.what())));
    }
    catch (...)
    {
        result.emplace(Result<DetectionResult>::failure(
            exception_error("algorithm.detector.process", frame.camera_id(), "unknown")));
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started);
    impl_->metrics.last_processing_time = elapsed;
    impl_->metrics.maximum_processing_time =
        (std::max)(impl_->metrics.maximum_processing_time, elapsed);

    if (elapsed > impl_->config->processing_timeout)
    {
        ++impl_->metrics.process_failures;
        ++impl_->metrics.process_timeouts;
        auto error = host_error("ALGORITHM_PROCESS_TIMEOUT", "检测器处理超过同步时间预算",
                                "algorithm.detector.process", frame.camera_id());
        error.details.push_back({"elapsedUs", std::to_string(elapsed.count())});
        error.details.push_back(
            {"budgetUs", std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
                                            impl_->config->processing_timeout)
                                            .count())});
        return Result<DetectionResult>::failure(std::move(error));
    }
    if (!*result)
    {
        ++impl_->metrics.process_failures;
        auto error = std::move(result->error());
        add_stage(error, "process");
        return Result<DetectionResult>::failure(std::move(error));
    }

    auto detection = std::move(*result).value();
    if (detection.camera_id != frame.camera_id() ||
        detection.sequence_number != frame.sequence_number())
    {
        ++impl_->metrics.process_failures;
        return Result<DetectionResult>::failure(
            host_error("ALGORITHM_PROCESS_FAILED", "检测器结果帧身份不一致",
                       "algorithm.detector.process", frame.camera_id()));
    }
    detection.processing_time = elapsed;
    if (detection.detector_version.empty())
    {
        detection.detector_version = impl_->active_info->implementation_version;
        detection.model_version = impl_->active_info->model_version;
    }
    ++impl_->metrics.process_successes;
    return Result<DetectionResult>::success(std::move(detection));
}

Result<void> DetectorHost::reset()
{
    if (!impl_->active || !impl_->config)
    {
        return Result<void>::failure(
            host_error("ALGORITHM_NOT_READY", "检测器尚未装载", "algorithm.detector.reset"));
    }
    ++impl_->metrics.reset_calls;
    try
    {
        auto result = impl_->active->reset();
        if (!result)
        {
            auto error = std::move(result.error());
            add_stage(error, "reset");
            return Result<void>::failure(std::move(error));
        }
        return Result<void>::success();
    }
    catch (const std::exception& exception)
    {
        return Result<void>::failure(exception_error("algorithm.detector.reset",
                                                     impl_->config->camera_id, "std::exception",
                                                     exception.what()));
    }
    catch (...)
    {
        return Result<void>::failure(
            exception_error("algorithm.detector.reset", impl_->config->camera_id, "unknown"));
    }
}

Result<DetectorInfo> DetectorHost::info() const
{
    if (!impl_->active || !impl_->config)
    {
        return Result<DetectorInfo>::failure(
            host_error("ALGORITHM_NOT_READY", "检测器尚未装载", "algorithm.detector.info"));
    }
    return invoke_info(*impl_->active, impl_->config->camera_id);
}

const DetectorConfig* DetectorHost::active_config() const noexcept
{
    return impl_->config ? &*impl_->config : nullptr;
}

DetectorHostMetrics DetectorHost::metrics() const noexcept
{
    return impl_->metrics;
}

} // namespace paperbreak::algorithm
