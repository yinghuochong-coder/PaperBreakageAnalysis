#include <gtest/gtest.h>

TEST(HardwareBaseline, RequiresApprovedTargetRig)
{
    GTEST_SKIP() << "M0 does not perform Hikrobot or physical-camera validation";
}
