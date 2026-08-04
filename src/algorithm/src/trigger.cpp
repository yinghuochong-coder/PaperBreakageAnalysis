#include "paperbreak/algorithm/trigger.hpp"

namespace paperbreak::algorithm
{

std::string_view to_string(const TriggerSource source) noexcept
{
    switch (source)
    {
    case TriggerSource::none:
        return "None";
    case TriggerSource::manual_test:
        return "ManualTest";
    case TriggerSource::fixed_period:
        return "FixedPeriod";
    case TriggerSource::mean_grayscale_change:
        return "MeanGrayscaleChange";
    case TriggerSource::roi_paper_ratio:
        return "RoiPaperRatio";
    case TriggerSource::background_change:
        return "BackgroundChange";
    }
    return "None";
}

} // namespace paperbreak::algorithm
