#include "mvs_lifecycle.hpp"

#include <MvErrorDefine.h>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

namespace paperbreak::camera::hikrobot::detail
{
namespace
{
struct FakeContext final
{
    int enumerate_code{MV_OK};
    int create_code{MV_OK};
    int open_code{MV_OK};
    int start_code{MV_OK};
    int stop_code{MV_OK};
    int close_code{MV_OK};
    int destroy_code{MV_OK};
    int handle_token{};
    MV_CC_DEVICE_INFO device_info{};
    std::vector<std::string> calls;
};

thread_local FakeContext* current_context{};

FakeContext& context()
{
    if (current_context == nullptr)
    {
        throw std::logic_error{"fake MVS context is not installed"};
    }
    return *current_context;
}

unsigned int __stdcall fake_sdk_version()
{
    return 0x04080003U;
}

int __stdcall fake_enumerate(const unsigned int, MV_CC_DEVICE_INFO_LIST* list)
{
    auto& state = context();
    state.calls.emplace_back("enumerate");
    if (state.enumerate_code == MV_OK)
    {
        list->nDeviceNum = 1U;
        list->pDeviceInfo[0] = &state.device_info;
    }
    return state.enumerate_code;
}

int __stdcall fake_create(void** handle, const MV_CC_DEVICE_INFO*)
{
    auto& state = context();
    state.calls.emplace_back("create");
    if (state.create_code == MV_OK)
    {
        *handle = &state.handle_token;
    }
    return state.create_code;
}

int __stdcall fake_open(void*, const unsigned int, const unsigned short)
{
    auto& state = context();
    state.calls.emplace_back("open");
    return state.open_code;
}

int __stdcall fake_close(void*)
{
    auto& state = context();
    state.calls.emplace_back("close");
    return state.close_code;
}

int __stdcall fake_destroy(void*)
{
    auto& state = context();
    state.calls.emplace_back("destroy");
    return state.destroy_code;
}

int __stdcall fake_start(void*)
{
    auto& state = context();
    state.calls.emplace_back("start");
    return state.start_code;
}

int __stdcall fake_stop(void*)
{
    auto& state = context();
    state.calls.emplace_back("stop");
    return state.stop_code;
}

const MvsApi fake_api{.get_sdk_version = &fake_sdk_version,
                      .enumerate_devices = &fake_enumerate,
                      .create_handle = &fake_create,
                      .open_device = &fake_open,
                      .close_device = &fake_close,
                      .destroy_handle = &fake_destroy,
                      .start_grabbing = &fake_start,
                      .stop_grabbing = &fake_stop};

class MvsLifecycleTest : public testing::Test
{
  protected:
    void SetUp() override
    {
        current_context = &context_;
    }

    void TearDown() override
    {
        current_context = nullptr;
    }

    FakeContext context_;
};

TEST_F(MvsLifecycleTest, DeviceListOwnsBoundedSdkListValue)
{
    auto result = DeviceList::enumerate(fake_api, MV_GIGE_DEVICE);

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().size(), 1U);
    ASSERT_NE(result.value().at(0U), nullptr);
    EXPECT_NE(result.value().at(0U), &context_.device_info);
    EXPECT_EQ(result.value().at(0U)->nTLayerType, context_.device_info.nTLayerType);
    EXPECT_EQ(result.value().at(1U), nullptr);
    EXPECT_EQ(context_.calls, std::vector<std::string>{"enumerate"});
}

TEST_F(MvsLifecycleTest, EnumerationFailureKeepsStableAndNativeCodes)
{
    context_.enumerate_code = MV_E_NETER;

    auto result = DeviceList::enumerate(fake_api, MV_GIGE_DEVICE);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().business_code, "CAMERA_NOT_FOUND");
    EXPECT_EQ(result.error().native_domain, "hikrobot-mvs");
    EXPECT_EQ(result.error().native_code, "0x80000206");
}

TEST_F(MvsLifecycleTest, CreateFailureDoesNotDestroyUnownedHandle)
{
    context_.create_code = MV_E_RESOURCE;

    auto result = DeviceHandle::open(fake_api, context_.device_info);

    ASSERT_FALSE(result);
    EXPECT_EQ(context_.calls, std::vector<std::string>{"create"});
    EXPECT_EQ(result.error().business_code, "CAMERA_OPEN_FAILED");
}

TEST_F(MvsLifecycleTest, OpenFailureDestroysCreatedHandleAndClassifiesAccessDenied)
{
    context_.open_code = MV_E_RESOURCE_IN_USE;

    auto result = DeviceHandle::open(fake_api, context_.device_info);

    ASSERT_FALSE(result);
    EXPECT_EQ(context_.calls, (std::vector<std::string>{"create", "open", "destroy"}));
    EXPECT_EQ(result.error().business_code, "CAMERA_ACCESS_DENIED");
    EXPECT_EQ(result.error().native_code, "0x80000028");
}

TEST_F(MvsLifecycleTest, StreamAndHandleDestructorsReleaseInReverseOrder)
{
    {
        auto handle_result = DeviceHandle::open(fake_api, context_.device_info);
        ASSERT_TRUE(handle_result);
        auto handle = std::move(handle_result).value();
        auto stream_result = handle.start_streaming();
        ASSERT_TRUE(stream_result);
        auto stream = std::move(stream_result).value();
        EXPECT_TRUE(stream.active());
    }

    EXPECT_EQ(context_.calls,
              (std::vector<std::string>{"create", "open", "start", "stop", "close", "destroy"}));
}

TEST_F(MvsLifecycleTest, ExplicitStopIsIdempotent)
{
    auto handle_result = DeviceHandle::open(fake_api, context_.device_info);
    ASSERT_TRUE(handle_result);
    auto handle = std::move(handle_result).value();
    auto stream_result = handle.start_streaming();
    ASSERT_TRUE(stream_result);
    auto stream = std::move(stream_result).value();

    EXPECT_TRUE(stream.stop());
    EXPECT_TRUE(stream.stop());
    EXPECT_TRUE(handle.close());
    EXPECT_TRUE(handle.close());

    EXPECT_EQ(context_.calls,
              (std::vector<std::string>{"create", "open", "start", "stop", "close", "destroy"}));
}

TEST_F(MvsLifecycleTest, StartFailureDoesNotCreateStreamingOwner)
{
    context_.start_code = MV_E_RESOURCE;
    auto handle_result = DeviceHandle::open(fake_api, context_.device_info);
    ASSERT_TRUE(handle_result);
    auto handle = std::move(handle_result).value();

    auto stream_result = handle.start_streaming();

    ASSERT_FALSE(stream_result);
    EXPECT_EQ(stream_result.error().business_code, "CAMERA_STREAM_START_FAILED");
}

TEST_F(MvsLifecycleTest, CleanupContinuesAfterStopFailure)
{
    context_.stop_code = MV_E_INTERNAL;
    {
        auto handle_result = DeviceHandle::open(fake_api, context_.device_info);
        ASSERT_TRUE(handle_result);
        auto handle = std::move(handle_result).value();
        auto stream_result = handle.start_streaming();
        ASSERT_TRUE(stream_result);
        auto stream = std::move(stream_result).value();

        auto stop_result = stream.stop();
        ASSERT_FALSE(stop_result);
        EXPECT_EQ(stop_result.error().native_code, "0x800000FE");
    }

    EXPECT_EQ(context_.calls, (std::vector<std::string>{"create", "open", "start", "stop", "stop",
                                                        "stop", "close", "destroy"}));
}

TEST(MvsCallbackBoundaryTest, TrampolineDeliversValidFrameWithoutThrowing)
{
    std::size_t delivered_size{};
    ImageCallbackBoundary boundary{
        [&delivered_size](const std::span<const std::byte> bytes, const MV_FRAME_OUT_INFO_EX&) {
            delivered_size = bytes.size();
        }};
    std::array<unsigned char, 4U> bytes{};
    MV_FRAME_OUT_INFO_EX info{};
    info.nFrameLen = static_cast<unsigned int>(bytes.size());

    image_callback_trampoline(bytes.data(), &info, &boundary);

    EXPECT_EQ(delivered_size, bytes.size());
    EXPECT_EQ(boundary.diagnostics().invocations, 1U);
    EXPECT_EQ(boundary.diagnostics().failures, 0U);
}

TEST(MvsCallbackBoundaryTest, CatchesStandardAndUnknownExceptions)
{
    std::array<unsigned char, 1U> bytes{};
    MV_FRAME_OUT_INFO_EX info{};
    info.nFrameLen = 1U;
    ImageCallbackBoundary standard_boundary{
        [](const std::span<const std::byte>, const MV_FRAME_OUT_INFO_EX&) {
            throw std::runtime_error{"test"};
        }};
    ImageCallbackBoundary unknown_boundary{
        [](const std::span<const std::byte>, const MV_FRAME_OUT_INFO_EX&) { throw 42; }};

    EXPECT_NO_THROW(image_callback_trampoline(bytes.data(), &info, &standard_boundary));
    EXPECT_NO_THROW(image_callback_trampoline(bytes.data(), &info, &unknown_boundary));

    EXPECT_EQ(standard_boundary.diagnostics().last_failure, CallbackFailure::standard_exception);
    EXPECT_EQ(unknown_boundary.diagnostics().last_failure, CallbackFailure::unknown_exception);
}

TEST(MvsSdkSmokeTest, ApprovedRuntimeVersionIsLoaded)
{
    EXPECT_EQ(production_mvs_api().get_sdk_version(), 0x04080003U);
}
} // namespace
} // namespace paperbreak::camera::hikrobot::detail
