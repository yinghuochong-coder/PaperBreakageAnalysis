#include "mvs_lifecycle.hpp"

#include <MvErrorDefine.h>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace paperbreak::camera::hikrobot::detail
{
namespace
{
struct FakeContext final
{
    struct FloatNode final
    {
        float current{};
        float minimum{};
        float maximum{};
    };
    struct IntegerNode final
    {
        std::int64_t current{};
        std::int64_t minimum{};
        std::int64_t maximum{};
        std::int64_t increment{1};
    };
    struct EnumNode final
    {
        unsigned int current{};
        std::vector<unsigned int> supported;
    };
    int enumerate_code{MV_OK};
    int create_code{MV_OK};
    int open_code{MV_OK};
    int start_code{MV_OK};
    int stop_code{MV_OK};
    int capture_code{MV_OK};
    int close_code{MV_OK};
    int destroy_code{MV_OK};
    int command_code{MV_OK};
    int handle_token{};
    unsigned int enumerate_transport{};
    bool accessible{true};
    MV_CC_DEVICE_INFO device_info{};
    MV_FRAME_OUT_INFO_EX frame_info{};
    std::vector<unsigned char> frame_payload;
    std::vector<std::string> calls;
    std::unordered_map<std::string, FloatNode> floats;
    std::unordered_map<std::string, IntegerNode> integers;
    std::unordered_map<std::string, EnumNode> enumerations;
    std::unordered_map<std::string, bool> booleans;
    std::unordered_map<std::string, float> forced_float_readback;
    std::string fail_get_node;
    int fail_get_code{MV_OK};
    std::string fail_set_node;
    int fail_set_code{MV_OK};
    std::size_t fail_set_remaining{};
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

int __stdcall fake_enumerate(const unsigned int transport, MV_CC_DEVICE_INFO_LIST* list)
{
    auto& state = context();
    state.calls.emplace_back("enumerate");
    state.enumerate_transport = transport;
    if (state.enumerate_code == MV_OK)
    {
        list->nDeviceNum = 1U;
        list->pDeviceInfo[0] = &state.device_info;
    }
    return state.enumerate_code;
}

bool __stdcall fake_is_accessible(MV_CC_DEVICE_INFO*, const unsigned int access_mode)
{
    auto& state = context();
    state.calls.emplace_back("accessible");
    EXPECT_EQ(access_mode, MV_ACCESS_Exclusive);
    return state.accessible;
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

int __stdcall fake_capture(void*, unsigned char* destination, const unsigned int capacity,
                           MV_FRAME_OUT_INFO_EX* info, const unsigned int timeout_ms)
{
    auto& state = context();
    state.calls.emplace_back("capture:" + std::to_string(timeout_ms));
    if (state.capture_code != MV_OK)
    {
        return state.capture_code;
    }
    if (state.frame_payload.size() > capacity)
    {
        return MV_E_BUFOVER;
    }
    std::copy(state.frame_payload.begin(), state.frame_payload.end(), destination);
    *info = state.frame_info;
    return MV_OK;
}

int __stdcall fake_get_float(void*, const char* node, MVCC_FLOATVALUE* value)
{
    auto& state = context();
    state.calls.emplace_back("getf:" + std::string{node});
    if (state.fail_get_node == node)
    {
        return state.fail_get_code;
    }
    const auto found = state.floats.find(node);
    if (found == state.floats.end())
        return MV_E_SUPPORT;
    value->fCurValue = found->second.current;
    value->fMin = found->second.minimum;
    value->fMax = found->second.maximum;
    return MV_OK;
}

int maybe_fail_set(const char* node)
{
    auto& state = context();
    if (state.fail_set_node == node && state.fail_set_remaining > 0U)
    {
        --state.fail_set_remaining;
        return state.fail_set_code;
    }
    return MV_OK;
}

int __stdcall fake_set_float(void*, const char* node, const float value)
{
    auto& state = context();
    state.calls.emplace_back("setf:" + std::string{node});
    if (const int code = maybe_fail_set(node); code != MV_OK)
        return code;
    const auto found = state.floats.find(node);
    if (found == state.floats.end())
        return MV_E_SUPPORT;
    const auto forced = state.forced_float_readback.find(node);
    found->second.current = forced == state.forced_float_readback.end() ? value : forced->second;
    return MV_OK;
}

int __stdcall fake_get_int(void*, const char* node, MVCC_INTVALUE_EX* value)
{
    auto& state = context();
    state.calls.emplace_back("geti:" + std::string{node});
    const auto found = state.integers.find(node);
    if (found == state.integers.end())
        return MV_E_SUPPORT;
    value->nCurValue = found->second.current;
    value->nMin = found->second.minimum;
    value->nMax = found->second.maximum;
    value->nInc = found->second.increment;
    return MV_OK;
}

int __stdcall fake_set_int(void*, const char* node, const std::int64_t value)
{
    auto& state = context();
    state.calls.emplace_back("seti:" + std::string{node});
    if (const int code = maybe_fail_set(node); code != MV_OK)
        return code;
    const auto found = state.integers.find(node);
    if (found == state.integers.end())
        return MV_E_SUPPORT;
    found->second.current = value;
    return MV_OK;
}

int __stdcall fake_get_enum(void*, const char* node, MVCC_ENUMVALUE* value)
{
    auto& state = context();
    state.calls.emplace_back("gete:" + std::string{node});
    const auto found = state.enumerations.find(node);
    if (found == state.enumerations.end())
        return MV_E_SUPPORT;
    value->nCurValue = found->second.current;
    value->nSupportedNum = static_cast<unsigned int>(found->second.supported.size());
    std::copy(found->second.supported.begin(), found->second.supported.end(), value->nSupportValue);
    return MV_OK;
}

int __stdcall fake_set_enum(void*, const char* node, const unsigned int value)
{
    auto& state = context();
    state.calls.emplace_back("sete:" + std::string{node});
    if (const int code = maybe_fail_set(node); code != MV_OK)
        return code;
    const auto found = state.enumerations.find(node);
    if (found == state.enumerations.end())
        return MV_E_SUPPORT;
    found->second.current = value;
    return MV_OK;
}

int __stdcall fake_get_bool(void*, const char* node, bool* value)
{
    auto& state = context();
    state.calls.emplace_back("getb:" + std::string{node});
    const auto found = state.booleans.find(node);
    if (found == state.booleans.end())
        return MV_E_SUPPORT;
    *value = found->second;
    return MV_OK;
}

int __stdcall fake_set_bool(void*, const char* node, const bool value)
{
    auto& state = context();
    state.calls.emplace_back("setb:" + std::string{node});
    if (const int code = maybe_fail_set(node); code != MV_OK)
        return code;
    const auto found = state.booleans.find(node);
    if (found == state.booleans.end())
        return MV_E_SUPPORT;
    found->second = value;
    return MV_OK;
}

int __stdcall fake_set_command(void*, const char* node)
{
    auto& state = context();
    state.calls.emplace_back("command:" + std::string{node});
    return state.command_code;
}

const MvsApi fake_api{.get_sdk_version = &fake_sdk_version,
                      .enumerate_devices = &fake_enumerate,
                      .is_device_accessible = &fake_is_accessible,
                      .create_handle = &fake_create,
                      .open_device = &fake_open,
                      .close_device = &fake_close,
                      .destroy_handle = &fake_destroy,
                      .start_grabbing = &fake_start,
                      .stop_grabbing = &fake_stop,
                      .get_one_frame_timeout = &fake_capture,
                      .get_float_value = &fake_get_float,
                      .set_float_value = &fake_set_float,
                      .get_int_value = &fake_get_int,
                      .set_int_value = &fake_set_int,
                      .get_enum_value = &fake_get_enum,
                      .set_enum_value = &fake_set_enum,
                      .get_bool_value = &fake_get_bool,
                      .set_bool_value = &fake_set_bool,
                      .set_command_value = &fake_set_command};

class MvsLifecycleTest : public testing::Test
{
  protected:
    void SetUp() override
    {
        current_context = &context_;
        context_.device_info.nTLayerType = MV_GIGE_DEVICE;
    }

    void TearDown() override
    {
        current_context = nullptr;
    }

    FakeContext context_;
};

template <std::size_t Size>
void set_sdk_text(unsigned char (&destination)[Size], const std::string_view value)
{
    ASSERT_LT(value.size(), Size);
    std::memcpy(destination, value.data(), value.size());
    destination[value.size()] = 0U;
}

void configure_gige_device(MV_CC_DEVICE_INFO& info, const std::string_view model,
                           const std::string_view serial)
{
    info.nTLayerType = MV_GIGE_DEVICE;
    set_sdk_text(info.SpecialInfo.stGigEInfo.chModelName, model);
    set_sdk_text(info.SpecialInfo.stGigEInfo.chSerialNumber, serial);
    info.SpecialInfo.stGigEInfo.nCurrentIp = 0xC0000263U;
    info.SpecialInfo.stGigEInfo.nNetExport = 0xC000020AU;
}

void configure_parameter_nodes(FakeContext& state)
{
    state.floats = {{"ExposureTime", {1000.0F, 10.0F, 100000.0F}},
                    {"Gain", {2.0F, 0.0F, 24.0F}},
                    {"AcquisitionFrameRate", {30.0F, 1.0F, 60.0F}},
                    {"TriggerDelay", {0.0F, 0.0F, 1000.0F}}};
    state.integers = {{"Width", {1600, 64, 1600, 4}},
                      {"Height", {1200, 64, 1200, 2}},
                      {"OffsetX", {0, 0, 1536, 4}},
                      {"OffsetY", {0, 0, 1136, 2}},
                      {"GevSCPSPacketSize", {1500, 576, 9000, 4}},
                      {"GevSCPD", {0, 0, 10000, 1}},
                      {"PayloadSize", {1920000, 1, 10000000, 1}}};
    state.enumerations = {
        {"PixelFormat", {PixelType_Gvsp_Mono8, {PixelType_Gvsp_Mono8, PixelType_Gvsp_Mono12}}},
        {"TriggerMode", {0U, {0U, 1U}}},
        {"TriggerSource", {0U, {0U, 7U}}}};
}

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

TEST_F(MvsLifecycleTest, ProviderEnumeratesOnlyGigeAndMapsNetworkAndOccupancy)
{
    configure_gige_device(context_.device_info, "MV-CS020-60GM", "SERIAL-0001");
    context_.accessible = false;
    HikrobotCameraProvider provider{fake_api};

    const auto result = provider.enumerate_devices();

    ASSERT_TRUE(result);
    ASSERT_EQ(result.value().size(), 1U);
    EXPECT_EQ(context_.enumerate_transport, MV_GIGE_DEVICE);
    EXPECT_EQ(result.value().front().model_name, "MV-CS020-60GM");
    EXPECT_EQ(result.value().front().serial_number, "SERIAL-0001");
    EXPECT_EQ(result.value().front().ip_address, "192.0.2.99");
    EXPECT_EQ(result.value().front().network_interface, "192.0.2.10");
    EXPECT_FALSE(result.value().front().exclusive_access_available);
    EXPECT_EQ(context_.calls, (std::vector<std::string>{"enumerate", "accessible"}));
}

TEST_F(MvsLifecycleTest, DescriptorMappingRejectsNonGigeAndUnterminatedFields)
{
    context_.device_info.nTLayerType = MV_USB_DEVICE;
    auto result = map_gige_descriptor(context_.device_info, true);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().details.front().value, "unexpected-transport-type");

    context_.device_info.nTLayerType = MV_GIGE_DEVICE;
    std::fill(std::begin(context_.device_info.SpecialInfo.stGigEInfo.chModelName),
              std::end(context_.device_info.SpecialInfo.stGigEInfo.chModelName),
              static_cast<unsigned char>('X'));
    result = map_gige_descriptor(context_.device_info, true);
    ASSERT_FALSE(result);
    ASSERT_EQ(result.error().details.size(), 2U);
    EXPECT_EQ(result.error().details.front().value, "modelName");
    EXPECT_EQ(result.error().details.back().value, "invalid-bounded-text");
}

TEST_F(MvsLifecycleTest, ProviderCreatesOnlyExactSerialAndPreservesOpenConflictCode)
{
    configure_gige_device(context_.device_info, "MV-CS020-60GM", "SERIAL-0001");
    HikrobotCameraProvider provider{fake_api};

    auto missing = provider.create_device("SERIAL-9999");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().business_code, "CAMERA_NOT_FOUND");

    context_.calls.clear();
    context_.open_code = MV_E_RESOURCE_IN_USE;
    auto created = provider.create_device("SERIAL-0001");
    ASSERT_TRUE(created);
    EXPECT_EQ(created.value()->descriptor().serial_number, "SERIAL-0001");
    auto connected = created.value()->connect();
    ASSERT_FALSE(connected);
    EXPECT_EQ(connected.error().business_code, "CAMERA_ACCESS_DENIED");
    EXPECT_EQ(connected.error().native_code, "0x80000028");
}

TEST_F(MvsLifecycleTest, CreateFailureDoesNotDestroyUnownedHandle)
{
    context_.create_code = MV_E_RESOURCE;

    auto result = DeviceHandle::open(fake_api, context_.device_info);

    ASSERT_FALSE(result);
    EXPECT_EQ(context_.calls, std::vector<std::string>{"create"});
    EXPECT_EQ(result.error().business_code, "CAMERA_OPEN_FAILED");
}

TEST_F(MvsLifecycleTest, MapsParameterCapabilitiesAndReadsCompleteSnapshot)
{
    configure_parameter_nodes(context_);
    auto opened = DeviceHandle::open(fake_api, context_.device_info);
    ASSERT_TRUE(opened);
    auto handle = std::move(opened).value();

    const auto capabilities = handle.capabilities();
    ASSERT_TRUE(capabilities);
    ASSERT_TRUE(capabilities.value().exposure_us);
    EXPECT_EQ(capabilities.value().exposure_us->increment, 0.0);
    EXPECT_EQ(capabilities.value().exposure_us->minimum, 10.0);
    ASSERT_TRUE(capabilities.value().roi);
    EXPECT_EQ(capabilities.value().roi->sensor_width, 1600U);
    EXPECT_EQ(capabilities.value().pixel_formats,
              (std::vector<PixelFormat>{PixelFormat::mono8, PixelFormat::mono12}));
    EXPECT_EQ(capabilities.value().trigger_modes,
              (std::vector<TriggerMode>{TriggerMode::continuous, TriggerMode::software,
                                        TriggerMode::hardware}));
    EXPECT_EQ(capabilities.value().trigger_sources, std::vector<std::string>{"Line0"});
    EXPECT_EQ(capabilities.value().maximum_payload_bytes, 1920000U);

    const auto snapshot = handle.read_parameters();
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().exposure_us, 1000.0);
    EXPECT_EQ(snapshot.value().roi, (Roi{1600U, 1200U, 0U, 0U}));
    EXPECT_EQ(snapshot.value().pixel_format, PixelFormat::mono8);
    EXPECT_EQ(snapshot.value().trigger_mode, TriggerMode::continuous);
    EXPECT_EQ(snapshot.value().packet_size_bytes, 1500U);
}

TEST_F(MvsLifecycleTest, ParameterApplyPausesWritesReadsBackAndResumesStreaming)
{
    configure_parameter_nodes(context_);
    auto opened = DeviceHandle::open(fake_api, context_.device_info);
    ASSERT_TRUE(opened);
    auto handle = std::move(opened).value();
    auto stream = handle.start_streaming();
    ASSERT_TRUE(stream);
    context_.forced_float_readback["ExposureTime"] = 2496.0F;
    context_.calls.clear();

    const auto applied = handle.apply_parameters({.exposure_us = 2500.0,
                                                  .roi = Roi{800U, 600U, 8U, 4U},
                                                  .pixel_format = PixelFormat::mono12});

    ASSERT_TRUE(applied);
    EXPECT_EQ(applied.value().exposure_us, 2496.0);
    EXPECT_EQ(applied.value().roi, (Roi{800U, 600U, 8U, 4U}));
    EXPECT_EQ(applied.value().pixel_format, PixelFormat::mono12);
    const auto stop = std::find(context_.calls.begin(), context_.calls.end(), "stop");
    const auto first_write =
        std::find(context_.calls.begin(), context_.calls.end(), "seti:OffsetX");
    const auto restart = std::find(context_.calls.begin(), context_.calls.end(), "start");
    ASSERT_NE(stop, context_.calls.end());
    ASSERT_NE(first_write, context_.calls.end());
    ASSERT_NE(restart, context_.calls.end());
    EXPECT_LT(stop, first_write);
    EXPECT_LT(first_write, restart);
    EXPECT_TRUE(std::move(stream).value().active());
}

TEST_F(MvsLifecycleTest, ParameterReadFailureUsesStableBusinessAndNativeDiagnostics)
{
    configure_parameter_nodes(context_);
    auto opened = DeviceHandle::open(fake_api, context_.device_info);
    ASSERT_TRUE(opened);
    auto handle = std::move(opened).value();
    context_.fail_get_node = "Gain";
    context_.fail_get_code = MV_E_NETER;

    const auto capabilities = handle.capabilities();

    ASSERT_FALSE(capabilities);
    EXPECT_EQ(capabilities.error().business_code, "CAMERA_PARAMETER_READ_FAILED");
    EXPECT_EQ(capabilities.error().native_domain, "hikrobot-mvs");
    EXPECT_EQ(capabilities.error().native_code, "0x80000206");
}

TEST_F(MvsLifecycleTest, WriteFailureRestoresOldSnapshotAndReturnsStableNativeError)
{
    configure_parameter_nodes(context_);
    auto opened = DeviceHandle::open(fake_api, context_.device_info);
    ASSERT_TRUE(opened);
    auto handle = std::move(opened).value();
    context_.fail_set_node = "Gain";
    context_.fail_set_code = MV_E_PARAMETER_RANGE;
    context_.fail_set_remaining = 1U;

    const auto applied = handle.apply_parameters({.exposure_us = 2500.0, .gain_db = 5.0});

    ASSERT_FALSE(applied);
    EXPECT_EQ(applied.error().business_code, "CAMERA_PARAMETER_WRITE_FAILED");
    EXPECT_EQ(applied.error().native_code, "0x80000025");
    const auto restored = handle.read_parameters();
    ASSERT_TRUE(restored);
    EXPECT_EQ(restored.value().exposure_us, 1000.0);
    EXPECT_EQ(restored.value().gain_db, 2.0);
}

TEST_F(MvsLifecycleTest, RollbackFailureLocksParameterSessionInFaultedState)
{
    configure_parameter_nodes(context_);
    auto opened = DeviceHandle::open(fake_api, context_.device_info);
    ASSERT_TRUE(opened);
    auto handle = std::move(opened).value();
    context_.fail_set_node = "Gain";
    context_.fail_set_code = MV_E_GC_ACCESS;
    context_.fail_set_remaining = 2U;

    const auto applied = handle.apply_parameters({.exposure_us = 2500.0, .gain_db = 5.0});

    ASSERT_FALSE(applied);
    EXPECT_EQ(applied.error().business_code, "CAMERA_PARAMETER_FAULTED");
    const auto read = handle.read_parameters();
    ASSERT_FALSE(read);
    EXPECT_EQ(read.error().business_code, "CAMERA_PARAMETER_FAULTED");
}

TEST_F(MvsLifecycleTest, ResumeFailureThatCannotRecoverLocksParameterSession)
{
    configure_parameter_nodes(context_);
    auto opened = DeviceHandle::open(fake_api, context_.device_info);
    ASSERT_TRUE(opened);
    auto handle = std::move(opened).value();
    auto stream = handle.start_streaming();
    ASSERT_TRUE(stream);
    context_.start_code = MV_E_NETER;

    const auto applied = handle.apply_parameters({.exposure_us = 2500.0});

    ASSERT_FALSE(applied);
    EXPECT_EQ(applied.error().business_code, "CAMERA_PARAMETER_FAULTED");
    EXPECT_FALSE(std::move(stream).value().active());
}

TEST_F(MvsLifecycleTest, MapsAndReadsWritableDigitalOutput)
{
    configure_parameter_nodes(context_);
    context_.enumerations["LineSelector"] = {1U, {1U}};
    context_.enumerations["LineMode"] = {1U, {0U, 1U}};
    context_.enumerations["UserOutputSelector"] = {1U, {1U}};
    context_.booleans["UserOutputValue"] = false;
    auto opened = DeviceHandle::open(fake_api, context_.device_info);
    ASSERT_TRUE(opened);
    auto handle = std::move(opened).value();

    const auto capabilities = handle.capabilities();
    ASSERT_TRUE(capabilities);
    ASSERT_EQ(capabilities.value().digital_io.size(), 1U);
    EXPECT_EQ(capabilities.value().digital_io.front(),
              (DigitalIoCapability{"Line1", DigitalIoDirection::output, true}));
    const auto applied = handle.apply_parameters({.digital_io = {{"Line1", true}}});
    ASSERT_TRUE(applied);
    ASSERT_EQ(applied.value().digital_io.size(), 1U);
    EXPECT_EQ(applied.value().digital_io.front(), (DigitalIoState{"Line1", true}));
}

TEST_F(MvsLifecycleTest, SoftwareTriggerChecksActualModeAndExecutesCommand)
{
    configure_parameter_nodes(context_);
    context_.enumerations["TriggerMode"].current = 1U;
    context_.enumerations["TriggerSource"].current = 7U;
    auto opened = DeviceHandle::open(fake_api, context_.device_info);
    ASSERT_TRUE(opened);
    auto handle = std::move(opened).value();
    context_.calls.clear();

    const auto triggered = handle.software_trigger();

    ASSERT_TRUE(triggered);
    EXPECT_EQ(context_.calls, (std::vector<std::string>{"gete:TriggerMode", "gete:TriggerSource",
                                                        "command:TriggerSoftware"}));
}

TEST_F(MvsLifecycleTest, SoftwareTriggerRejectsActualModeMismatchWithoutCommand)
{
    configure_parameter_nodes(context_);
    auto opened = DeviceHandle::open(fake_api, context_.device_info);
    ASSERT_TRUE(opened);
    auto handle = std::move(opened).value();
    context_.calls.clear();

    const auto triggered = handle.software_trigger();

    ASSERT_FALSE(triggered);
    EXPECT_EQ(triggered.error().business_code, "CAMERA_INVALID_STATE_TRANSITION");
    EXPECT_EQ(context_.calls, (std::vector<std::string>{"gete:TriggerMode", "gete:TriggerSource"}));
}

TEST_F(MvsLifecycleTest, SoftwareTriggerPreservesCommandNativeFailure)
{
    configure_parameter_nodes(context_);
    context_.enumerations["TriggerMode"].current = 1U;
    context_.enumerations["TriggerSource"].current = 7U;
    context_.command_code = MV_E_NETER;
    auto opened = DeviceHandle::open(fake_api, context_.device_info);
    ASSERT_TRUE(opened);
    auto handle = std::move(opened).value();

    const auto triggered = handle.software_trigger();

    ASSERT_FALSE(triggered);
    EXPECT_EQ(triggered.error().business_code, "CAMERA_PARAMETER_WRITE_FAILED");
    EXPECT_EQ(triggered.error().native_domain, "hikrobot-mvs");
    EXPECT_EQ(triggered.error().native_code, "0x80000206");
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

TEST_F(MvsLifecycleTest, CapturesIntoPreallocatedBufferAndMapsFrameMetadata)
{
    context_.integers["GevTimestampTickFrequency"] = {1000000000, 1, 1000000000, 1};
    context_.frame_payload = {1U, 2U, 3U, 4U};
    context_.frame_info.nWidth = 2U;
    context_.frame_info.nHeight = 2U;
    context_.frame_info.enPixelType = PixelType_Gvsp_Mono8;
    context_.frame_info.nFrameNum = 73U;
    context_.frame_info.nDevTimeStampHigh = 1U;
    context_.frame_info.nDevTimeStampLow = 2U;
    context_.frame_info.nFrameLen = 4U;
    context_.frame_info.nLostPacket = 3U;
    auto handle_result = DeviceHandle::open(fake_api, context_.device_info);
    ASSERT_TRUE(handle_result);
    auto handle = std::move(handle_result).value();
    auto stream_result = handle.start_streaming();
    ASSERT_TRUE(stream_result);
    auto stream = std::move(stream_result).value();
    FrameBuffer buffer{8U};

    auto captured = handle.capture_into(buffer, std::chrono::milliseconds{25});

    ASSERT_TRUE(captured);
    EXPECT_EQ(buffer.size(), 4U);
    EXPECT_EQ(captured.value().camera_frame_number, 73U);
    EXPECT_EQ(captured.value().geometry, (FrameGeometry{2U, 2U, 2U}));
    EXPECT_EQ(captured.value().pixel_format, PixelFormat::mono8);
    EXPECT_TRUE(captured.value().flags.incomplete);
    ASSERT_TRUE(captured.value().camera_timestamp);
    EXPECT_EQ(captured.value().camera_timestamp->ticks, (std::uint64_t{1U} << 32U) | 2U);
    EXPECT_EQ(captured.value().camera_timestamp->frequency_hz, 1000000000U);
    EXPECT_EQ(captured.value().camera_timestamp->quality, CameraTimestampQuality::unsynchronized);
    EXPECT_NE(std::find(context_.calls.begin(), context_.calls.end(), "capture:25"),
              context_.calls.end());
}

TEST_F(MvsLifecycleTest, MapsCaptureTimeoutAndDisconnectWithNativeDiagnostics)
{
    auto handle_result = DeviceHandle::open(fake_api, context_.device_info);
    ASSERT_TRUE(handle_result);
    auto handle = std::move(handle_result).value();
    auto stream_result = handle.start_streaming();
    ASSERT_TRUE(stream_result);
    auto stream = std::move(stream_result).value();
    FrameBuffer buffer{8U};

    context_.capture_code = MV_E_NODATA;
    auto result = handle.capture_into(buffer, std::chrono::milliseconds{10});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().business_code, "CAMERA_FRAME_TIMEOUT");
    EXPECT_EQ(result.error().native_domain, "hikrobot-mvs");
    EXPECT_EQ(result.error().native_code, "0x80000007");

    context_.capture_code = MV_E_NETER;
    result = handle.capture_into(buffer, std::chrono::milliseconds{10});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().business_code, "CAMERA_DISCONNECTED");
    EXPECT_EQ(result.error().native_code, "0x80000206");
}

TEST_F(MvsLifecycleTest, RejectsUnsupportedOrInvalidFrameLayoutWithoutAllocation)
{
    context_.frame_payload = {1U, 2U, 3U, 4U};
    context_.frame_info.nWidth = 2U;
    context_.frame_info.nHeight = 2U;
    context_.frame_info.nFrameLen = 4U;
    context_.frame_info.enPixelType = PixelType_Gvsp_Mono10_Packed;
    auto handle_result = DeviceHandle::open(fake_api, context_.device_info);
    ASSERT_TRUE(handle_result);
    auto handle = std::move(handle_result).value();
    auto stream_result = handle.start_streaming();
    ASSERT_TRUE(stream_result);
    auto stream = std::move(stream_result).value();
    FrameBuffer buffer{4U};

    auto result = handle.capture_into(buffer, std::chrono::milliseconds{10});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().business_code, "CAMERA_FRAME_FORMAT_CHANGED");
    EXPECT_EQ(buffer.capacity(), 4U);

    context_.frame_info.enPixelType = PixelType_Gvsp_Mono8;
    context_.frame_info.nFrameLen = 3U;
    context_.frame_info.nFrameLenEx = 3U;
    result = handle.capture_into(buffer, std::chrono::milliseconds{10});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().business_code, "CAMERA_FRAME_FORMAT_CHANGED");
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
