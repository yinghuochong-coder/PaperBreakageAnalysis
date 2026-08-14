#include "mvs_lifecycle.hpp"

#include <MvErrorDefine.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
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
    std::vector<std::pair<std::string, unsigned int>> enum_writes;
    std::unordered_map<std::string, FloatNode> floats;
    std::unordered_map<std::string, IntegerNode> integers;
    std::unordered_map<std::string, EnumNode> enumerations;
    std::unordered_map<std::string, bool> booleans;
    std::unordered_map<std::string, std::pair<MvEventCallback, void*>> event_callbacks;
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
    if (state.fail_get_node == node)
        return state.fail_get_code;
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
    state.enum_writes.emplace_back(node, value);
    if (const int code = maybe_fail_set(node); code != MV_OK)
        return code;
    const auto found = state.enumerations.find(node);
    if (found == state.enumerations.end())
        return MV_E_SUPPORT;
    found->second.current = value;
    return MV_OK;
}

int __stdcall fake_set_enum_by_string(void*, const char* node, const char* value)
{
    auto& state = context();
    state.calls.emplace_back("setes:" + std::string{node} + "=" + value);
    if (const int code = maybe_fail_set(node); code != MV_OK)
        return code;
    if (std::string_view{node} != "ExposureAuto")
        return MV_OK;
    const auto found = state.enumerations.find(node);
    if (found == state.enumerations.end())
        return MV_E_SUPPORT;
    if (std::string_view{value} == "Off")
        found->second.current = 0U;
    else if (std::string_view{value} == "Once")
        found->second.current = 1U;
    else if (std::string_view{value} == "Continuous")
        found->second.current = 2U;
    else
        return MV_E_PARAMETER;
    return MV_OK;
}

int __stdcall fake_get_bool(void*, const char* node, bool* value)
{
    auto& state = context();
    state.calls.emplace_back("getb:" + std::string{node});
    if (state.fail_get_node == node)
        return state.fail_get_code;
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

int __stdcall fake_register_event(void*, const char* event, MvEventCallback callback, void* user)
{
    auto& state = context();
    state.calls.emplace_back("register:" + std::string{event});
    if (const int code = maybe_fail_set(event); code != MV_OK)
        return code;
    state.event_callbacks[event] = {callback, user};
    return MV_OK;
}

int __stdcall fake_event_on(void*, const char* event)
{
    auto& state = context();
    state.calls.emplace_back("event-on:" + std::string{event});
    return maybe_fail_set(event);
}

int __stdcall fake_event_off(void*, const char* event)
{
    auto& state = context();
    state.calls.emplace_back("event-off:" + std::string{event});
    return maybe_fail_set(event);
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
                      .set_enum_value_by_string = &fake_set_enum_by_string,
                      .get_bool_value = &fake_get_bool,
                      .set_bool_value = &fake_set_bool,
                      .set_command_value = &fake_set_command,
                      .register_event_callback = &fake_register_event,
                      .event_notification_on = &fake_event_on,
                      .event_notification_off = &fake_event_off};

class MvsLifecycleTest : public testing::Test
{
  protected:
    void SetUp() override
    {
        current_context = &context_;
        context_.device_info.nTLayerType = MV_GIGE_DEVICE;
        context_.enumerations["ExposureAuto"] = {0U, {0U, 1U, 2U}};
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
    state.integers = {{"WidthMax", {1600, 1600, 1600, 0}},
                      {"HeightMax", {1200, 1200, 1200, 0}},
                      {"SensorWidth", {1600, 1600, 1600, 0}},
                      {"SensorHeight", {1200, 1200, 1200, 0}},
                      {"Width", {1600, 64, 1600, 4}},
                      {"Height", {1200, 64, 1200, 2}},
                      {"OffsetX", {0, 0, 1536, 4}},
                      {"OffsetY", {0, 0, 1136, 2}},
                      {"GevSCPSPacketSize", {1500, 576, 9000, 4}},
                      {"GevSCPD", {400, 0, 10000, 1}},
                      {"GevTimestampTickFrequency", {125000000, 1, 1000000000, 1}},
                      {"PayloadSize", {1920000, 1, 10000000, 1}}};
    state.enumerations = {
        {"ExposureAuto", {0U, {0U, 1U, 2U}}},
        {"PixelFormat", {PixelType_Gvsp_Mono8, {PixelType_Gvsp_Mono8, PixelType_Gvsp_Mono12}}},
        {"TriggerMode", {0U, {0U, 1U}}},
        {"TriggerSource", {0U, {0U, 7U}}}};
    state.booleans = {
        {"ReverseX", false}, {"ReverseY", true}, {"AcquisitionFrameRateEnable", false}};
}

void configure_line_io_nodes(FakeContext& state)
{
    state.enumerations["LineSelector"] = {0U, {0U, 1U}};
    state.enumerations["LineMode"] = {0U, {0U, 8U}};
    state.integers["StrobeLineDuration"] = {0, 1, 1000000, 1};
    state.integers["StrobeLinePreDelay"] = {0, 0, 100000, 2};
    state.integers["StrobeLineDelay"] = {0, 0, 100000, 1};
    state.booleans["LineStatus"] = false;
    state.booleans["StrobeEnable"] = false;
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
    EXPECT_EQ(capabilities.value().exposure_auto_modes,
              (std::vector<ExposureAutoMode>{ExposureAutoMode::off, ExposureAutoMode::once,
                                             ExposureAutoMode::continuous}));
    ASSERT_TRUE(capabilities.value().roi);
    EXPECT_EQ(capabilities.value().roi->sensor_width, 1600U);
    EXPECT_TRUE(capabilities.value().supports_reverse_x);
    EXPECT_TRUE(capabilities.value().supports_reverse_y);
    EXPECT_EQ(capabilities.value().pixel_formats,
              (std::vector<PixelFormat>{PixelFormat::mono8, PixelFormat::mono12}));
    EXPECT_EQ(capabilities.value().trigger_modes,
              (std::vector<TriggerMode>{TriggerMode::continuous, TriggerMode::software,
                                        TriggerMode::hardware}));
    EXPECT_EQ(capabilities.value().trigger_sources, std::vector<std::string>{"Line0"});
    EXPECT_EQ(capabilities.value().maximum_payload_bytes, 1920000U);
    ASSERT_TRUE(capabilities.value().inter_packet_delay_ns);
    EXPECT_EQ(*capabilities.value().inter_packet_delay_ns,
              (SteppedRange<std::uint32_t>{0U, 80000U, 8U}));

    const auto snapshot = handle.read_parameters();
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().exposure_us, 1000.0);
    EXPECT_EQ(snapshot.value().exposure_auto_mode, ExposureAutoMode::off);
    EXPECT_EQ(snapshot.value().roi, (Roi{1600U, 1200U, 0U, 0U}));
    EXPECT_EQ(snapshot.value().reverse_x, false);
    EXPECT_EQ(snapshot.value().reverse_y, true);
    EXPECT_EQ(snapshot.value().pixel_format, PixelFormat::mono8);
    EXPECT_EQ(snapshot.value().trigger_mode, TriggerMode::continuous);
    EXPECT_EQ(snapshot.value().packet_size_bytes, 1500U);
    EXPECT_EQ(snapshot.value().inter_packet_delay_ns, 3200U);
}

TEST_F(MvsLifecycleTest, ConvertsPacketDelayNanosecondsToDeviceTicksAndReadsBack)
{
    configure_parameter_nodes(context_);
    auto opened = DeviceHandle::open(fake_api, context_.device_info);
    ASSERT_TRUE(opened);
    auto handle = std::move(opened).value();

    const auto applied = handle.apply_parameters({.inter_packet_delay_ns = 400U});

    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_EQ(context_.integers.at("GevSCPD").current, 50);
    EXPECT_EQ(applied.value().inter_packet_delay_ns, 400U);
}

TEST_F(MvsLifecycleTest, RejectsPacketDelayNotAlignedToDeviceTickBeforeWrite)
{
    configure_parameter_nodes(context_);
    auto opened = DeviceHandle::open(fake_api, context_.device_info);
    ASSERT_TRUE(opened);
    auto handle = std::move(opened).value();
    context_.calls.clear();

    const auto applied = handle.apply_parameters({.inter_packet_delay_ns = 50U});

    ASSERT_FALSE(applied);
    EXPECT_EQ(applied.error().business_code, "CAMERA_CONFIG_FAILED");
    EXPECT_EQ(std::count(context_.calls.begin(), context_.calls.end(), "seti:GevSCPD"), 0);
    EXPECT_EQ(context_.integers.at("GevSCPD").current, 400);
}

TEST_F(MvsLifecycleTest, OneGigahertzPacketDelayFrequencyKeepsNanosecondsOneToOne)
{
    configure_parameter_nodes(context_);
    context_.integers.at("GevTimestampTickFrequency").current = 1000000000;
    auto opened = DeviceHandle::open(fake_api, context_.device_info);
    ASSERT_TRUE(opened);
    auto handle = std::move(opened).value();

    const auto snapshot = handle.read_parameters();
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().inter_packet_delay_ns, 400U);
    const auto applied = handle.apply_parameters({.inter_packet_delay_ns = 400U});
    ASSERT_TRUE(applied);
    EXPECT_EQ(context_.integers.at("GevSCPD").current, 400);
}

TEST_F(MvsLifecycleTest, PacketDelayRequiresTimestampFrequency)
{
    configure_parameter_nodes(context_);
    context_.integers.erase("GevTimestampTickFrequency");
    auto opened = DeviceHandle::open(fake_api, context_.device_info);
    ASSERT_TRUE(opened);
    auto handle = std::move(opened).value();

    const auto capabilities = handle.capabilities();

    ASSERT_FALSE(capabilities);
    EXPECT_EQ(capabilities.error().business_code, "CAMERA_PARAMETER_READ_FAILED");
    ASSERT_FALSE(capabilities.error().details.empty());
    EXPECT_EQ(capabilities.error().details.back().value, "timestamp-frequency-unavailable");
}

TEST_F(MvsLifecycleTest, PacketDelayRejectsInvalidTimestampFrequencies)
{
    for (const std::int64_t frequency : {0LL, 3LL, 2000000000LL})
    {
        configure_parameter_nodes(context_);
        context_.integers.at("GevTimestampTickFrequency").current = frequency;
        auto opened = DeviceHandle::open(fake_api, context_.device_info);
        ASSERT_TRUE(opened);
        auto handle = std::move(opened).value();

        const auto capabilities = handle.capabilities();

        ASSERT_FALSE(capabilities);
        EXPECT_EQ(capabilities.error().business_code, "CAMERA_PARAMETER_READ_FAILED");
        ASSERT_FALSE(capabilities.error().details.empty());
        EXPECT_EQ(capabilities.error().details.back().value,
                  "timestamp-frequency-not-integral-nanoseconds");
        ASSERT_TRUE(handle.close());
    }
}

TEST_F(MvsLifecycleTest, PacketDelayRejectsNanosecondRangeOverflow)
{
    configure_parameter_nodes(context_);
    context_.integers.at("GevSCPD").maximum = std::numeric_limits<std::uint32_t>::max();
    auto opened = DeviceHandle::open(fake_api, context_.device_info);
    ASSERT_TRUE(opened);
    auto handle = std::move(opened).value();

    const auto capabilities = handle.capabilities();

    ASSERT_FALSE(capabilities);
    EXPECT_EQ(capabilities.error().business_code, "CAMERA_PARAMETER_READ_FAILED");
    ASSERT_FALSE(capabilities.error().details.empty());
    EXPECT_EQ(capabilities.error().details.back().value, "delay-conversion-overflow");
}

TEST_F(MvsLifecycleTest, PacketDelayRollbackConvertsNanosecondsBackToOriginalTicks)
{
    configure_parameter_nodes(context_);
    auto opened = DeviceHandle::open(fake_api, context_.device_info);
    ASSERT_TRUE(opened);
    auto handle = std::move(opened).value();
    context_.fail_set_node = "TriggerSource";
    context_.fail_set_code = MV_E_PARAMETER_RANGE;
    context_.fail_set_remaining = 1U;

    const auto applied = handle.apply_parameters(
        {.trigger_mode = TriggerMode::software, .inter_packet_delay_ns = 400U});

    ASSERT_FALSE(applied);
    EXPECT_EQ(applied.error().business_code, "CAMERA_PARAMETER_WRITE_FAILED");
    EXPECT_EQ(context_.integers.at("GevSCPD").current, 400);
    const auto restored = handle.read_parameters();
    ASSERT_TRUE(restored);
    EXPECT_EQ(restored.value().inter_packet_delay_ns, 3200U);
}

TEST_F(MvsLifecycleTest, ExpandsDynamicVerticalOffsetRangeForSmallerRequestedHeight)
{
    configure_parameter_nodes(context_);
    context_.integers["Height"].current = 600;
    context_.integers["Height"].maximum = 600;
    context_.integers["OffsetY"].maximum = 0;
    auto opened = DeviceHandle::open(fake_api, context_.device_info);
    ASSERT_TRUE(opened);
    auto handle = std::move(opened).value();

    const auto capabilities = handle.capabilities();
    ASSERT_TRUE(capabilities);
    ASSERT_TRUE(capabilities.value().roi);
    EXPECT_EQ(capabilities.value().roi->sensor_height, 1200U);
    EXPECT_EQ(capabilities.value().roi->height.maximum, 1200U);
    EXPECT_EQ(capabilities.value().roi->offset_y.maximum, 1136U);

    const auto applied = handle.apply_parameters({.roi = Roi{1600U, 600U, 0U, 4U}});

    ASSERT_TRUE(applied);
    EXPECT_EQ(applied.value().roi, (Roi{1600U, 600U, 0U, 4U}));
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
                                                  .exposure_auto_mode = ExposureAutoMode::off,
                                                  .roi = Roi{800U, 600U, 8U, 4U},
                                                  .reverse_x = true,
                                                  .reverse_y = false,
                                                  .pixel_format = PixelFormat::mono12});

    ASSERT_TRUE(applied);
    EXPECT_EQ(applied.value().exposure_us, 2496.0);
    EXPECT_EQ(applied.value().roi, (Roi{800U, 600U, 8U, 4U}));
    EXPECT_EQ(applied.value().reverse_x, true);
    EXPECT_EQ(applied.value().reverse_y, false);
    EXPECT_EQ(applied.value().pixel_format, PixelFormat::mono12);
    const auto stop = std::find(context_.calls.begin(), context_.calls.end(), "stop");
    const auto first_write =
        std::find(context_.calls.begin(), context_.calls.end(), "seti:OffsetX");
    const auto disable_auto =
        std::find(context_.calls.begin(), context_.calls.end(), "setes:ExposureAuto=Off");
    const auto exposure_write =
        std::find(context_.calls.begin(), context_.calls.end(), "setf:ExposureTime");
    const auto restart = std::find(context_.calls.begin(), context_.calls.end(), "start");
    ASSERT_NE(stop, context_.calls.end());
    ASSERT_NE(first_write, context_.calls.end());
    ASSERT_NE(disable_auto, context_.calls.end());
    ASSERT_NE(exposure_write, context_.calls.end());
    ASSERT_NE(restart, context_.calls.end());
    const auto keep_fixed_exposure =
        std::find(exposure_write, context_.calls.end(), "setes:ExposureAuto=Off");
    ASSERT_NE(keep_fixed_exposure, context_.calls.end());
    EXPECT_LT(stop, first_write);
    EXPECT_LT(disable_auto, exposure_write);
    EXPECT_LT(exposure_write, keep_fixed_exposure);
    EXPECT_LT(keep_fixed_exposure, restart);
    EXPECT_LT(first_write, restart);
    EXPECT_TRUE(std::move(stream).value().active());
}

TEST_F(MvsLifecycleTest, AutoExposureWritesBaselineThenContinuousModeAndReadsBack)
{
    configure_parameter_nodes(context_);
    auto opened = DeviceHandle::open(fake_api, context_.device_info);
    ASSERT_TRUE(opened);
    auto handle = std::move(opened).value();
    context_.calls.clear();

    const auto applied = handle.apply_parameters(
        {.exposure_us = 2500.0, .exposure_auto_mode = ExposureAutoMode::continuous});

    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_EQ(applied.value().exposure_auto_mode, ExposureAutoMode::continuous);
    EXPECT_EQ(context_.enumerations.at("ExposureAuto").current, 2U);
    const auto disabled =
        std::find(context_.calls.begin(), context_.calls.end(), "setes:ExposureAuto=Off");
    const auto exposure =
        std::find(context_.calls.begin(), context_.calls.end(), "setf:ExposureTime");
    const auto enabled =
        std::find(context_.calls.begin(), context_.calls.end(), "setes:ExposureAuto=Continuous");
    ASSERT_NE(disabled, context_.calls.end());
    ASSERT_NE(exposure, context_.calls.end());
    ASSERT_NE(enabled, context_.calls.end());
    EXPECT_LT(disabled, exposure);
    EXPECT_LT(exposure, enabled);
}

TEST_F(MvsLifecycleTest, FrameRateEnableIsWrittenBeforeRateAndReadBack)
{
    configure_parameter_nodes(context_);
    auto opened = DeviceHandle::open(fake_api, context_.device_info);
    ASSERT_TRUE(opened);
    auto handle = std::move(opened).value();
    context_.calls.clear();

    const auto applied = handle.apply_parameters({.frame_rate = 25.0});

    ASSERT_TRUE(applied);
    EXPECT_EQ(applied.value().frame_rate, 25.0);
    EXPECT_TRUE(context_.booleans.at("AcquisitionFrameRateEnable"));
    const auto enable =
        std::find(context_.calls.begin(), context_.calls.end(), "setb:AcquisitionFrameRateEnable");
    const auto rate =
        std::find(context_.calls.begin(), context_.calls.end(), "setf:AcquisitionFrameRate");
    ASSERT_NE(enable, context_.calls.end());
    ASSERT_NE(rate, context_.calls.end());
    EXPECT_LT(enable, rate);
    EXPECT_NE(std::find(rate, context_.calls.end(), "getb:AcquisitionFrameRateEnable"),
              context_.calls.end());
}

TEST_F(MvsLifecycleTest, FrameRateEnableExplicitlyUnsupportedUsesDirectRateWrite)
{
    configure_parameter_nodes(context_);
    context_.booleans.erase("AcquisitionFrameRateEnable");
    auto opened = DeviceHandle::open(fake_api, context_.device_info);
    ASSERT_TRUE(opened);
    auto handle = std::move(opened).value();
    context_.calls.clear();

    const auto applied = handle.apply_parameters({.frame_rate = 20.0});

    ASSERT_TRUE(applied);
    EXPECT_EQ(applied.value().frame_rate, 20.0);
    EXPECT_EQ(
        std::find(context_.calls.begin(), context_.calls.end(), "setb:AcquisitionFrameRateEnable"),
        context_.calls.end());
}

TEST_F(MvsLifecycleTest, FrameRateEnableReadFailureIsNotTreatedAsUnsupported)
{
    configure_parameter_nodes(context_);
    auto opened = DeviceHandle::open(fake_api, context_.device_info);
    ASSERT_TRUE(opened);
    auto handle = std::move(opened).value();
    context_.fail_get_node = "AcquisitionFrameRateEnable";
    context_.fail_get_code = MV_E_GC_ACCESS;

    const auto applied = handle.apply_parameters({.frame_rate = 20.0});

    ASSERT_FALSE(applied);
    EXPECT_EQ(applied.error().business_code, "CAMERA_PARAMETER_READ_FAILED");
    EXPECT_EQ(applied.error().native_code, "0x80000106");
}

TEST_F(MvsLifecycleTest, FrameRateWriteFailureRestoresRateAndEnableState)
{
    configure_parameter_nodes(context_);
    auto opened = DeviceHandle::open(fake_api, context_.device_info);
    ASSERT_TRUE(opened);
    auto handle = std::move(opened).value();
    context_.fail_set_node = "AcquisitionFrameRate";
    context_.fail_set_code = MV_E_PARAMETER_RANGE;
    context_.fail_set_remaining = 1U;

    const auto applied = handle.apply_parameters({.frame_rate = 20.0});

    ASSERT_FALSE(applied);
    EXPECT_EQ(applied.error().business_code, "CAMERA_PARAMETER_WRITE_FAILED");
    EXPECT_FLOAT_EQ(context_.floats.at("AcquisitionFrameRate").current, 30.0F);
    EXPECT_FALSE(context_.booleans.at("AcquisitionFrameRateEnable"));
    const auto restored = handle.read_parameters();
    ASSERT_TRUE(restored);
    EXPECT_EQ(restored.value().frame_rate, 30.0);
}

TEST_F(MvsLifecycleTest, FrameRateEnableWriteFailureRestoresCompleteTransaction)
{
    configure_parameter_nodes(context_);
    auto opened = DeviceHandle::open(fake_api, context_.device_info);
    ASSERT_TRUE(opened);
    auto handle = std::move(opened).value();
    context_.fail_set_node = "AcquisitionFrameRateEnable";
    context_.fail_set_code = MV_E_GC_ACCESS;
    context_.fail_set_remaining = 1U;

    const auto applied = handle.apply_parameters({.frame_rate = 20.0});

    ASSERT_FALSE(applied);
    EXPECT_EQ(applied.error().business_code, "CAMERA_PARAMETER_WRITE_FAILED");
    EXPECT_FLOAT_EQ(context_.floats.at("AcquisitionFrameRate").current, 30.0F);
    EXPECT_FALSE(context_.booleans.at("AcquisitionFrameRateEnable"));
    const auto restored = handle.read_parameters();
    ASSERT_TRUE(restored);
    EXPECT_EQ(restored.value().frame_rate, 30.0);
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
    context_.enumerations.at("ExposureAuto").current = 2U;
    auto opened = DeviceHandle::open(fake_api, context_.device_info);
    ASSERT_TRUE(opened);
    auto handle = std::move(opened).value();
    context_.fail_set_node = "Gain";
    context_.fail_set_code = MV_E_PARAMETER_RANGE;
    context_.fail_set_remaining = 1U;

    const auto applied = handle.apply_parameters(
        {.exposure_us = 2500.0, .exposure_auto_mode = ExposureAutoMode::once, .gain_db = 5.0});

    ASSERT_FALSE(applied);
    EXPECT_EQ(applied.error().business_code, "CAMERA_PARAMETER_WRITE_FAILED");
    EXPECT_EQ(applied.error().native_code, "0x80000025");
    const auto restored = handle.read_parameters();
    ASSERT_TRUE(restored);
    EXPECT_EQ(restored.value().exposure_us, 1000.0);
    EXPECT_EQ(restored.value().exposure_auto_mode, ExposureAutoMode::continuous);
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

TEST_F(MvsLifecycleTest, MapsLineZeroEventsAndLineOneStrobeRangesAndReadback)
{
    configure_parameter_nodes(context_);
    configure_line_io_nodes(context_);
    auto opened = DeviceHandle::open(fake_api, context_.device_info);
    ASSERT_TRUE(opened);
    auto handle = std::move(opened).value();

    const auto capabilities = handle.capabilities();
    ASSERT_TRUE(capabilities) << capabilities.error().message;
    EXPECT_TRUE(capabilities.value().line_io.alarm_input_supported);
    EXPECT_TRUE(capabilities.value().line_io.line0_rising_edge_supported);
    EXPECT_TRUE(capabilities.value().line_io.line0_falling_edge_supported);
    EXPECT_TRUE(capabilities.value().line_io.strobe_output_supported);
    ASSERT_TRUE(capabilities.value().line_io.strobe_duration_us);
    EXPECT_EQ(capabilities.value().line_io.strobe_duration_us->minimum, 1U);
    EXPECT_EQ(capabilities.value().line_io.strobe_duration_us->maximum, 1000000U);
    ASSERT_TRUE(capabilities.value().line_io.strobe_pre_delay_us);
    EXPECT_EQ(capabilities.value().line_io.strobe_pre_delay_us->minimum, 0U);
    EXPECT_EQ(capabilities.value().line_io.strobe_pre_delay_us->increment, 2U);

    std::vector<bool> levels;
    handle.set_line_input_observer(
        [&](const LineInputEvent& event) { levels.push_back(event.raw_level); });
    context_.calls.clear();
    const auto applied =
        handle.apply_parameters({.line_io = LineIoParameters{.alarm_input_enabled = true,
                                                             .strobe_output_enabled = true,
                                                             .strobe_duration_us = 250U,
                                                             .strobe_pre_delay_us = 10U,
                                                             .strobe_post_delay_us = 20U}});

    ASSERT_TRUE(applied) << applied.error().message;
    ASSERT_TRUE(applied.value().line_io);
    EXPECT_TRUE(applied.value().line_io->alarm_input_enabled);
    EXPECT_TRUE(applied.value().line_io->strobe_output_enabled);
    EXPECT_EQ(applied.value().line_io->strobe_duration_us, 250U);
    EXPECT_EQ(applied.value().line_io->strobe_pre_delay_us, 10U);
    EXPECT_EQ(applied.value().line_io->strobe_post_delay_us, 20U);
    EXPECT_NE(std::find(context_.calls.begin(), context_.calls.end(), "register:Line0RisingEdge"),
              context_.calls.end());
    EXPECT_NE(std::find(context_.calls.begin(), context_.calls.end(), "event-on:Line0FallingEdge"),
              context_.calls.end());
    EXPECT_NE(std::find(context_.calls.begin(), context_.calls.end(),
                        "setes:LineSource=ExposureStartActive"),
              context_.calls.end());
    EXPECT_NE(std::find(context_.enum_writes.begin(), context_.enum_writes.end(),
                        std::pair<std::string, unsigned int>{"LineMode", 8U}),
              context_.enum_writes.end());
    EXPECT_NE(std::find(context_.calls.begin(), context_.calls.end(), "seti:StrobeLineDuration"),
              context_.calls.end());
    EXPECT_NE(std::find(context_.calls.begin(), context_.calls.end(), "seti:StrobeLinePreDelay"),
              context_.calls.end());
    EXPECT_NE(std::find(context_.calls.begin(), context_.calls.end(), "seti:StrobeLineDelay"),
              context_.calls.end());
    EXPECT_EQ(std::count_if(context_.calls.begin(), context_.calls.end(),
                            [](const auto& call) {
                                return call.starts_with("getf:StrobeLine") ||
                                       call.starts_with("setf:StrobeLine");
                            }),
              0);

    MV_EVENT_OUT_INFO rising{};
    constexpr char rising_name[] = "Line0RisingEdge";
    std::memcpy(rising.EventName, rising_name, sizeof(rising_name));
    const auto rising_callback = context_.event_callbacks.at("Line0RisingEdge");
    EXPECT_NO_THROW(rising_callback.first(&rising, rising_callback.second));
    MV_EVENT_OUT_INFO falling{};
    constexpr char falling_name[] = "Line0FallingEdge";
    std::memcpy(falling.EventName, falling_name, sizeof(falling_name));
    const auto falling_callback = context_.event_callbacks.at("Line0FallingEdge");
    EXPECT_NO_THROW(falling_callback.first(&falling, falling_callback.second));
    EXPECT_EQ(levels, (std::vector<bool>{true, false}));
}

TEST_F(MvsLifecycleTest, EnablesFixedLineZeroInputWithoutWritingReadOnlyLineMode)
{
    configure_parameter_nodes(context_);
    configure_line_io_nodes(context_);
    auto opened = DeviceHandle::open(fake_api, context_.device_info);
    ASSERT_TRUE(opened);
    auto handle = std::move(opened).value();
    handle.set_line_input_observer([](const LineInputEvent&) {});
    context_.calls.clear();

    const auto applied =
        handle.apply_parameters({.line_io = LineIoParameters{.alarm_input_enabled = true}});

    ASSERT_TRUE(applied) << applied.error().message;
    ASSERT_TRUE(applied.value().line_io);
    EXPECT_TRUE(applied.value().line_io->alarm_input_enabled);
    EXPECT_EQ(std::count(context_.calls.begin(), context_.calls.end(), "sete:LineMode"), 0);
    EXPECT_NE(std::find(context_.calls.begin(), context_.calls.end(), "getb:LineStatus"),
              context_.calls.end());
}

TEST_F(MvsLifecycleTest, LineIoFailureRollsBackEventsAndStrobeState)
{
    configure_parameter_nodes(context_);
    configure_line_io_nodes(context_);
    auto opened = DeviceHandle::open(fake_api, context_.device_info);
    ASSERT_TRUE(opened);
    auto handle = std::move(opened).value();
    context_.fail_set_node = "StrobeLineDuration";
    context_.fail_set_code = MV_E_PARAMETER_RANGE;
    context_.fail_set_remaining = 1U;

    const auto applied =
        handle.apply_parameters({.line_io = LineIoParameters{.alarm_input_enabled = true,
                                                             .strobe_output_enabled = true,
                                                             .strobe_duration_us = 250U}});

    ASSERT_FALSE(applied);
    EXPECT_EQ(applied.error().business_code, "CAMERA_PARAMETER_WRITE_FAILED");
    const auto restored = handle.read_parameters();
    ASSERT_TRUE(restored) << restored.error().message;
    ASSERT_TRUE(restored.value().line_io);
    EXPECT_FALSE(restored.value().line_io->alarm_input_enabled);
    EXPECT_FALSE(restored.value().line_io->strobe_output_enabled);
    EXPECT_EQ(context_.integers.at("StrobeLineDuration").current, 0);
    EXPECT_NE(std::find(context_.calls.begin(), context_.calls.end(), "event-off:Line0RisingEdge"),
              context_.calls.end());
}

TEST_F(MvsLifecycleTest, LineEventRegistrationFailureReturnsStableCameraError)
{
    configure_parameter_nodes(context_);
    configure_line_io_nodes(context_);
    auto opened = DeviceHandle::open(fake_api, context_.device_info);
    ASSERT_TRUE(opened);
    auto handle = std::move(opened).value();
    context_.fail_set_node = "Line0RisingEdge";
    context_.fail_set_code = MV_E_GC_ACCESS;
    context_.fail_set_remaining = 1U;

    const auto applied =
        handle.apply_parameters({.line_io = LineIoParameters{.alarm_input_enabled = true}});

    ASSERT_FALSE(applied);
    EXPECT_EQ(applied.error().business_code, "CAMERA_PARAMETER_WRITE_FAILED");
    EXPECT_EQ(applied.error().native_domain, "hikrobot-mvs");
    EXPECT_EQ(applied.error().native_code, "0x80000106");
    const auto restored = handle.read_parameters();
    ASSERT_TRUE(restored);
    ASSERT_TRUE(restored.value().line_io);
    EXPECT_FALSE(restored.value().line_io->alarm_input_enabled);
}

TEST_F(MvsLifecycleTest, CloseDisablesLineEventsBeforeStoppingAndClosingDevice)
{
    configure_parameter_nodes(context_);
    configure_line_io_nodes(context_);
    auto opened = DeviceHandle::open(fake_api, context_.device_info);
    ASSERT_TRUE(opened);
    auto handle = std::move(opened).value();
    ASSERT_TRUE(
        handle.apply_parameters({.line_io = LineIoParameters{.alarm_input_enabled = true}}));
    auto streaming = handle.start_streaming();
    ASSERT_TRUE(streaming);
    context_.calls.clear();

    ASSERT_TRUE(handle.close());

    const auto event_off =
        std::find(context_.calls.begin(), context_.calls.end(), "event-off:Line0FallingEdge");
    const auto stop = std::find(context_.calls.begin(), context_.calls.end(), "stop");
    const auto close = std::find(context_.calls.begin(), context_.calls.end(), "close");
    ASSERT_NE(event_off, context_.calls.end());
    ASSERT_NE(stop, context_.calls.end());
    ASSERT_NE(close, context_.calls.end());
    EXPECT_LT(event_off, stop);
    EXPECT_LT(stop, close);
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

    EXPECT_EQ(context_.calls, (std::vector<std::string>{"create", "open", "gete:ExposureAuto",
                                                        "start", "stop", "close", "destroy"}));
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

    EXPECT_EQ(context_.calls, (std::vector<std::string>{"create", "open", "gete:ExposureAuto",
                                                        "start", "stop", "close", "destroy"}));
}

TEST_F(MvsLifecycleTest, StartPreservesConfirmedContinuousAutoExposure)
{
    context_.enumerations.at("ExposureAuto").current = 2U;
    auto handle_result = DeviceHandle::open(fake_api, context_.device_info);
    ASSERT_TRUE(handle_result);
    auto handle = std::move(handle_result).value();

    auto stream_result = handle.start_streaming();

    ASSERT_TRUE(stream_result);
    const auto auto_exposure =
        std::find(context_.calls.begin(), context_.calls.end(), "gete:ExposureAuto");
    const auto start = std::find(context_.calls.begin(), context_.calls.end(), "start");
    ASSERT_NE(auto_exposure, context_.calls.end());
    ASSERT_NE(start, context_.calls.end());
    EXPECT_LT(auto_exposure, start);
    EXPECT_EQ(context_.enumerations.at("ExposureAuto").current, 2U);
    EXPECT_EQ(std::find(context_.calls.begin(), context_.calls.end(), "setes:ExposureAuto=Off"),
              context_.calls.end());
}

TEST_F(MvsLifecycleTest, UnconfirmedAutoExposureModePreventsGrabbing)
{
    context_.fail_get_node = "ExposureAuto";
    context_.fail_get_code = MV_E_GC_ACCESS;
    auto handle_result = DeviceHandle::open(fake_api, context_.device_info);
    ASSERT_TRUE(handle_result);
    auto handle = std::move(handle_result).value();

    auto stream_result = handle.start_streaming();

    ASSERT_FALSE(stream_result);
    EXPECT_EQ(stream_result.error().business_code, "CAMERA_STREAM_START_FAILED");
    EXPECT_EQ(stream_result.error().details.at(0).value, "ExposureAuto");
    EXPECT_EQ(std::find(context_.calls.begin(), context_.calls.end(), "start"),
              context_.calls.end());
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

    EXPECT_EQ(context_.calls,
              (std::vector<std::string>{"create", "open", "gete:ExposureAuto", "start", "stop",
                                        "stop", "stop", "close", "destroy"}));
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

TEST_F(MvsLifecycleTest, MissingDeviceTicksRemainUnavailableAtAdapterBoundary)
{
    context_.integers["GevTimestampTickFrequency"] = {1000000000, 1, 1000000000, 1};
    context_.frame_payload = {1U, 2U, 3U, 4U};
    context_.frame_info.nWidth = 2U;
    context_.frame_info.nHeight = 2U;
    context_.frame_info.enPixelType = PixelType_Gvsp_Mono8;
    context_.frame_info.nFrameNum = 1U;
    context_.frame_info.nDevTimeStampHigh = 0U;
    context_.frame_info.nDevTimeStampLow = 0U;
    context_.frame_info.nFrameLen = 4U;
    auto handle_result = DeviceHandle::open(fake_api, context_.device_info);
    ASSERT_TRUE(handle_result);
    auto handle = std::move(handle_result).value();
    auto stream_result = handle.start_streaming();
    ASSERT_TRUE(stream_result);
    auto stream = std::move(stream_result).value();
    FrameBuffer buffer{4U};

    const auto captured = handle.capture_into(buffer, std::chrono::milliseconds{10});

    ASSERT_TRUE(captured);
    EXPECT_FALSE(captured.value().camera_timestamp);
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

TEST(MvsCallbackBoundaryTest, LineEventBoundaryParsesEdgesAndContainsExceptions)
{
    std::vector<bool> levels;
    LineEventCallbackBoundary boundary{[&](const bool level) { levels.push_back(level); }};
    MV_EVENT_OUT_INFO rising{};
    constexpr char rising_name[] = "Line0RisingEdge";
    std::memcpy(rising.EventName, rising_name, sizeof(rising_name));
    MV_EVENT_OUT_INFO falling{};
    constexpr char falling_name[] = "Line0FallingEdge";
    std::memcpy(falling.EventName, falling_name, sizeof(falling_name));
    EXPECT_NO_THROW(line_event_callback_trampoline(&rising, &boundary));
    EXPECT_NO_THROW(line_event_callback_trampoline(&falling, &boundary));
    EXPECT_EQ(levels, (std::vector<bool>{true, false}));
    EXPECT_EQ(boundary.diagnostics().failures, 0U);

    LineEventCallbackBoundary throwing{[](const bool) { throw std::runtime_error{"test"}; }};
    EXPECT_NO_THROW(line_event_callback_trampoline(&rising, &throwing));
    EXPECT_EQ(throwing.diagnostics().last_failure, CallbackFailure::standard_exception);
    EXPECT_NO_THROW(line_event_callback_trampoline(nullptr, &throwing));
    EXPECT_EQ(throwing.diagnostics().failures, 2U);
}

TEST(MvsSdkSmokeTest, ApprovedRuntimeVersionIsLoaded)
{
    EXPECT_EQ(production_mvs_api().get_sdk_version(), 0x04080003U);
}

TEST(MvsSdkSmokeTest, GigeEnumerationRuntimeCanBeLoadedWithoutOpeningDevices)
{
    const auto result = DeviceList::enumerate(production_mvs_api(), MV_GIGE_DEVICE);

    ASSERT_TRUE(result) << result.error().message
                        << " [native=" << result.error().native_code.value_or("unknown") << ']';
}
} // namespace
} // namespace paperbreak::camera::hikrobot::detail
