#include "paperbreak/common/error.hpp"
#include "paperbreak/common/result.hpp"
#include "paperbreak/common/version.hpp"

#include <gtest/gtest.h>

#include <regex>
#include <string>

TEST(Result, RetainsBusinessNativeAndContextFields)
{
    auto error = paperbreak::make_error("CAMERA_OPEN_FAILED", paperbreak::Severity::error,
                                        "camera open failed", "camera", "camera.open", true);
    error.native_domain = "hikrobot-mvs";
    error.native_code = "0x80000203";
    error.details.push_back({"attempt", "2"});

    auto result = paperbreak::Result<int>::failure(std::move(error));

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().business_code, "CAMERA_OPEN_FAILED");
    ASSERT_TRUE(result.error().native_code.has_value());
    EXPECT_EQ(result.error().native_code.value(), "0x80000203");
    ASSERT_EQ(result.error().details.size(), 1U);
    EXPECT_EQ(result.error().details.front().key, "attempt");
}

TEST(Result, CarriesSuccessfulMoveOnlyValue)
{
    auto result = paperbreak::Result<std::string>::success("ready");
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value(), "ready");
}

TEST(CommonError, UsesStableUtcTimestampShape)
{
    const auto timestamp = paperbreak::current_utc_timestamp();
    const std::regex expected{R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$)"};
    EXPECT_TRUE(std::regex_match(timestamp, expected));
}

TEST(Version, ContainsBuildAndDependencyMetadata)
{
    const auto& version = paperbreak::version_info();
    EXPECT_FALSE(version.application_version.empty());
    EXPECT_FALSE(version.git_commit.empty());
    EXPECT_FALSE(version.build_time_utc.empty());
    EXPECT_EQ(version.qt_version, "6.10.2");
    EXPECT_EQ(version.opencv_version, "4.12.0");
    EXPECT_FALSE(paperbreak::format_version_info().empty());
}
