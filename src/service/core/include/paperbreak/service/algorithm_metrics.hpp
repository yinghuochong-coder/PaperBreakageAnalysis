#pragma once

#include "paperbreak/monitoring/monitoring.hpp"

#include <memory>

namespace paperbreak::service
{

class EventRuntime;

[[nodiscard]] std::shared_ptr<monitoring::IMetricSource> make_algorithm_metric_source(
    std::weak_ptr<EventRuntime> runtime);

} // namespace paperbreak::service
