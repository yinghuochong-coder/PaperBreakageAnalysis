#include "paperbreak/platform/windows_clock_probe.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <stop_token>

namespace
{
using namespace std::chrono_literals;

class FakeWindowsClockBackend final : public paperbreak::platform::IWindowsClockProbeBackend
{
  public:
    paperbreak::Result<paperbreak::platform::WindowsClockObservation> observe(
        std::stop_token, std::chrono::steady_clock::time_point) override
    {
        ++calls;
        if (failure)
            return paperbreak::Result<paperbreak::platform::WindowsClockObservation>::failure(
                *failure);
        return paperbreak::Result<paperbreak::platform::WindowsClockObservation>::success(value);
    }

    paperbreak::platform::WindowsClockObservation value{.time_service_installed = true,
                                                        .time_service_running = true,
                                                        .sample_monotonic_ns = 10,
                                                        .sample_utc_ns = 20,
                                                        .system_time_increment_ns = 1'000'000};
    std::optional<paperbreak::Error> failure;
    std::size_t calls{};
};
} // namespace

TEST(WindowsClockProbe, ReportsConservativeOsNtpWithoutClaimingHardwarePtp)
{
    auto backend = std::make_unique<FakeWindowsClockBackend>();
    auto* observed = backend.get();
    paperbreak::platform::WindowsSystemClockProbe probe{std::move(backend), 50'000'000};
    const auto sample = probe.sample({}, std::chrono::steady_clock::now() + 1s);
    ASSERT_TRUE(sample) << sample.error().message;
    EXPECT_EQ(observed->calls, 1U);
    EXPECT_EQ(sample.value().clock_source, paperbreak::time::ClockSource::ntp);
    EXPECT_EQ(sample.value().sync_state, paperbreak::time::SyncState::syncing);
    EXPECT_EQ(sample.value().uncertainty_ns, 50'000'000);
    EXPECT_FALSE(sample.value().offset_ns);
    EXPECT_EQ(sample.value().last_error_code, "TIME_SYNC_SOURCE_UNVERIFIED");
}

TEST(WindowsClockProbe, DistinguishesUnsupportedStoppedAndBackendFailure)
{
    auto unsupported_backend = std::make_unique<FakeWindowsClockBackend>();
    unsupported_backend->value.time_service_installed = false;
    paperbreak::platform::WindowsSystemClockProbe unsupported{std::move(unsupported_backend)};
    const auto unsupported_result = unsupported.sample({}, std::chrono::steady_clock::now() + 1s);
    ASSERT_FALSE(unsupported_result);
    EXPECT_EQ(unsupported_result.error().business_code, "TIME_PROBE_NOT_SUPPORTED");
    EXPECT_FALSE(unsupported_result.error().retryable);

    auto stopped_backend = std::make_unique<FakeWindowsClockBackend>();
    stopped_backend->value.time_service_running = false;
    paperbreak::platform::WindowsSystemClockProbe stopped{std::move(stopped_backend)};
    const auto stopped_result = stopped.sample({}, std::chrono::steady_clock::now() + 1s);
    ASSERT_FALSE(stopped_result);
    EXPECT_EQ(stopped_result.error().business_code, "TIME_PROBE_UNAVAILABLE");
    EXPECT_TRUE(stopped_result.error().retryable);

    auto failed_backend = std::make_unique<FakeWindowsClockBackend>();
    failed_backend->failure =
        paperbreak::make_error("TIME_PROBE_UNAVAILABLE", paperbreak::Severity::warning, "injected",
                               "test", "test.windowsClock", true);
    paperbreak::platform::WindowsSystemClockProbe failed{std::move(failed_backend)};
    const auto failed_result = failed.sample({}, std::chrono::steady_clock::now() + 1s);
    ASSERT_FALSE(failed_result);
    EXPECT_EQ(failed_result.error().business_code, "TIME_PROBE_UNAVAILABLE");
}

TEST(WindowsClockProbe, RejectsInvalidObservationAndConfiguration)
{
    auto backend = std::make_unique<FakeWindowsClockBackend>();
    backend->value.sample_utc_ns = 0;
    paperbreak::platform::WindowsSystemClockProbe invalid_observation{std::move(backend)};
    EXPECT_FALSE(invalid_observation.sample({}, std::chrono::steady_clock::now() + 1s));

    paperbreak::platform::WindowsSystemClockProbe invalid_configuration{nullptr, -1};
    const auto invalid = invalid_configuration.sample({}, std::chrono::steady_clock::now() + 1s);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().business_code, "TIME_PROBE_UNAVAILABLE");
}
