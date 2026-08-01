#include <gtest/gtest.h>

TEST(SimulationBaseline, RunsWithoutCameraHardware)
{
    SUCCEED() << "M0 simulation lane intentionally has no camera dependency";
}
