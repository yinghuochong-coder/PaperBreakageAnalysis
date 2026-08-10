#pragma once

#include "paperbreak/common/result.hpp"
#include "paperbreak/common/threading.hpp"
#include "paperbreak/storage/metadata_database.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>

namespace paperbreak::uplink
{

inline constexpr std::uint32_t maximum_upload_attempts = 100U;

enum class UploadAttemptDisposition
{
    succeeded,
    retryable_failure,
    permanent_failure,
    manual_intervention,
};

struct UploadAttemptOutcome final
{
    UploadAttemptDisposition disposition{UploadAttemptDisposition::succeeded};
    std::string checkpoint_json{"{}"};
    std::string error_code;
};

using UploadJobExecutor =
    std::function<UploadAttemptOutcome(const storage::UploadJobRecord&, std::stop_token)>;

struct PersistentUploadSchedulerConfig final
{
    std::chrono::milliseconds initial_retry_delay{std::chrono::seconds{1}};
    std::chrono::milliseconds maximum_retry_delay{std::chrono::minutes{5}};
    std::chrono::milliseconds idle_poll_interval{std::chrono::seconds{1}};
    double jitter_ratio{0.2};
    std::uint32_t maximum_attempts{10U};
    ThreadRegistrationFactory register_thread;
    DebugDiagnosticSink diagnostics;
    std::function<void(std::string_view event_id, std::string_view error_code)>
        integrity_failure_observer;
};

enum class PersistentUploadSchedulerState
{
    stopped,
    running,
    stop_requested,
};

struct PersistentUploadSchedulerSnapshot final
{
    PersistentUploadSchedulerState state{PersistentUploadSchedulerState::stopped};
    std::uint64_t recovered_jobs{};
    std::uint64_t claimed_jobs{};
    std::uint64_t completed_jobs{};
    std::uint64_t retryable_failures{};
    std::uint64_t permanent_failures{};
    std::uint64_t manual_interventions{};
    std::uint64_t repository_failures{};
    std::string last_error_code;
    std::int64_t active_job_id{};
};

/// Computes the bounded exponential delay for a one-based attempt number.
/// jitter_unit is clamped to [0,1] and maps linearly to [-jitter_ratio,+jitter_ratio].
[[nodiscard]] std::chrono::milliseconds upload_retry_delay(
    const PersistentUploadSchedulerConfig& config, std::uint32_t attempt_number,
    double jitter_unit) noexcept;

/// Executes persistent upload work on one deterministic worker.
///
/// The injected executor owns the M8-04 transport adaptation and must honor the stop token. This
/// class never calls camera callbacks and never creates an in-memory task queue.
class PersistentUploadScheduler final
{
  private:
    struct Impl;
    struct ValidatedTag final
    {
    };

  public:
    ~PersistentUploadScheduler();

    PersistentUploadScheduler(const PersistentUploadScheduler&) = delete;
    PersistentUploadScheduler& operator=(const PersistentUploadScheduler&) = delete;
    PersistentUploadScheduler(PersistentUploadScheduler&&) = delete;
    PersistentUploadScheduler& operator=(PersistentUploadScheduler&&) = delete;

    [[nodiscard]] static Result<std::unique_ptr<PersistentUploadScheduler>> create(
        std::shared_ptr<storage::EventMetadataDatabase> database,
        PersistentUploadSchedulerConfig config, UploadJobExecutor executor);

    PersistentUploadScheduler(ValidatedTag, std::shared_ptr<Impl> impl);

    [[nodiscard]] Result<storage::UploadJobEnqueueOutcome> enqueue(
        const storage::UploadJobRequest& request);
    [[nodiscard]] Result<void> retry(std::int64_t job_id);
    [[nodiscard]] Result<void> start();
    void request_stop() noexcept;
    [[nodiscard]] Result<void> join(std::chrono::steady_clock::time_point deadline);
    [[nodiscard]] PersistentUploadSchedulerSnapshot snapshot() const noexcept;

  private:
    std::shared_ptr<Impl> impl_;
};

[[nodiscard]] std::string_view persistent_upload_scheduler_state_name(
    PersistentUploadSchedulerState state) noexcept;

} // namespace paperbreak::uplink
