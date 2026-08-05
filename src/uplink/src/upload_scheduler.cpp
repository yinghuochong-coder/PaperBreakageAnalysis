#include "paperbreak/uplink/upload_scheduler.hpp"

#include "paperbreak/common/error.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <random>
#include <thread>
#include <utility>

namespace paperbreak::uplink
{
namespace
{

Error scheduler_error(std::string code, std::string message, std::string operation,
                      const bool retryable = false)
{
    return make_error(std::move(code), Severity::error, std::move(message), "uplink",
                      std::move(operation), retryable);
}

std::int64_t now_utc_ms() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool valid_config(const PersistentUploadSchedulerConfig& config) noexcept
{
    return config.initial_retry_delay.count() > 0 &&
           config.maximum_retry_delay >= config.initial_retry_delay &&
           config.maximum_retry_delay <= std::chrono::hours{24} &&
           config.idle_poll_interval.count() > 0 &&
           config.idle_poll_interval <= std::chrono::minutes{1} &&
           std::isfinite(config.jitter_ratio) && config.jitter_ratio >= 0.0 &&
           config.jitter_ratio <= 0.5 && config.maximum_attempts > 0U &&
           config.maximum_attempts <= maximum_upload_attempts;
}

} // namespace

std::chrono::milliseconds upload_retry_delay(const PersistentUploadSchedulerConfig& config,
                                             const std::uint32_t attempt_number,
                                             const double jitter_unit) noexcept
{
    auto base = config.initial_retry_delay;
    for (std::uint32_t attempt = 1U;
         attempt < std::max(1U, attempt_number) && base < config.maximum_retry_delay; ++attempt)
    {
        if (base > config.maximum_retry_delay / 2)
            base = config.maximum_retry_delay;
        else
            base *= 2;
    }
    base = std::min(base, config.maximum_retry_delay);
    const auto unit = std::clamp(std::isfinite(jitter_unit) ? jitter_unit : 0.5, 0.0, 1.0);
    const auto factor = 1.0 + config.jitter_ratio * (2.0 * unit - 1.0);
    const auto jittered = static_cast<long double>(base.count()) * factor;
    const auto bounded =
        std::clamp(jittered, 1.0L, static_cast<long double>(config.maximum_retry_delay.count()));
    return std::chrono::milliseconds{static_cast<std::int64_t>(bounded)};
}

struct PersistentUploadScheduler::Impl final
{
    std::shared_ptr<storage::EventMetadataDatabase> database;
    PersistentUploadSchedulerConfig config;
    UploadJobExecutor executor;

    mutable std::mutex mutex;
    std::condition_variable condition;
    std::stop_source stop_source;
    std::thread worker;
    PersistentUploadSchedulerSnapshot metrics;
    std::mt19937_64 random{
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())};
    bool started{};
    bool finished{true};

    bool stop_requested() const noexcept
    {
        return stop_source.stop_requested();
    }

    void record_repository_failure(const Error& error)
    {
        std::lock_guard lock{mutex};
        ++metrics.repository_failures;
        metrics.last_error_code = error.business_code;
    }

    bool interruptible_wait()
    {
        std::unique_lock lock{mutex};
        condition.wait_for(lock, config.idle_poll_interval);
        return !stop_requested();
    }

    void persist_outcome(const storage::UploadJobRecord& job, UploadAttemptOutcome outcome)
    {
        const auto now = now_utc_ms();
        Result<void> persisted = Result<void>::success();
        if (outcome.disposition == UploadAttemptDisposition::succeeded)
        {
            persisted = database->complete_upload_job(job.job_id, outcome.checkpoint_json, now);
            if (persisted)
            {
                std::lock_guard lock{mutex};
                ++metrics.completed_jobs;
            }
        }
        else
        {
            if (outcome.error_code.empty())
                outcome.error_code = "UPLOAD_TRANSFER_FAILED";
            auto failure_class = storage::UploadFailureClass::manual_intervention;
            std::optional<std::int64_t> next_attempt;
            if (outcome.disposition == UploadAttemptDisposition::retryable_failure &&
                (job.attempts < config.maximum_attempts || stop_requested()))
            {
                failure_class = storage::UploadFailureClass::retryable;
                std::uniform_real_distribution<double> distribution{0.0, 1.0};
                const auto delay = stop_requested() ? std::chrono::milliseconds{0}
                                                    : upload_retry_delay(config, job.attempts,
                                                                         distribution(random));
                next_attempt = now + delay.count();
                std::lock_guard lock{mutex};
                ++metrics.retryable_failures;
            }
            else if (outcome.disposition == UploadAttemptDisposition::permanent_failure)
            {
                failure_class = storage::UploadFailureClass::permanent;
                std::lock_guard lock{mutex};
                ++metrics.permanent_failures;
            }
            else
            {
                if (outcome.disposition == UploadAttemptDisposition::retryable_failure &&
                    job.attempts >= config.maximum_attempts)
                    outcome.error_code = "UPLOAD_RETRY_EXHAUSTED";
                std::lock_guard lock{mutex};
                ++metrics.manual_interventions;
            }
            persisted = database->fail_upload_job(job.job_id, failure_class, outcome.error_code,
                                                  outcome.checkpoint_json, next_attempt, now);
        }
        if (!persisted)
        {
            record_repository_failure(persisted.error());
            auto recovered = database->recover_upload_jobs(now);
            if (!recovered)
                record_repository_failure(recovered.error());
        }
    }

    void run() noexcept
    {
        try
        {
            auto recovered = database->recover_upload_jobs(now_utc_ms());
            if (!recovered)
                record_repository_failure(recovered.error());
            else
            {
                std::lock_guard lock{mutex};
                metrics.recovered_jobs += recovered.value();
            }

            while (!stop_requested())
            {
                auto claimed = database->claim_next_upload_job(now_utc_ms());
                if (!claimed)
                {
                    record_repository_failure(claimed.error());
                    if (!interruptible_wait())
                        break;
                    continue;
                }
                if (!claimed.value())
                {
                    if (!interruptible_wait())
                        break;
                    continue;
                }
                auto job = std::move(*claimed.value());
                {
                    std::lock_guard lock{mutex};
                    ++metrics.claimed_jobs;
                    metrics.active_job_id = job.job_id;
                }
                UploadAttemptOutcome outcome{.disposition =
                                                 UploadAttemptDisposition::manual_intervention,
                                             .checkpoint_json = job.checkpoint_json,
                                             .error_code = "SYS_INTERNAL_ERROR"};
                try
                {
                    outcome = executor(job, stop_source.get_token());
                }
                catch (...)
                {
                    if (stop_requested())
                    {
                        outcome.disposition = UploadAttemptDisposition::retryable_failure;
                        outcome.error_code = "UPLOAD_TRANSFER_INTERRUPTED";
                    }
                }
                persist_outcome(job, std::move(outcome));
                {
                    std::lock_guard lock{mutex};
                    metrics.active_job_id = 0;
                }
            }
        }
        catch (...)
        {
            std::lock_guard lock{mutex};
            metrics.last_error_code = "SYS_INTERNAL_ERROR";
        }
        {
            std::lock_guard lock{mutex};
            metrics.active_job_id = 0;
            metrics.state = PersistentUploadSchedulerState::stopped;
            finished = true;
        }
        condition.notify_all();
    }
};

PersistentUploadScheduler::PersistentUploadScheduler(ValidatedTag, std::shared_ptr<Impl> impl)
    : impl_(std::move(impl))
{
}

PersistentUploadScheduler::~PersistentUploadScheduler()
{
    request_stop();
    if (impl_->worker.joinable())
        impl_->worker.join();
}

Result<std::unique_ptr<PersistentUploadScheduler>> PersistentUploadScheduler::create(
    std::shared_ptr<storage::EventMetadataDatabase> database,
    PersistentUploadSchedulerConfig config, UploadJobExecutor executor)
{
    if (!database || !executor || !valid_config(config))
        return Result<std::unique_ptr<PersistentUploadScheduler>>::failure(
            scheduler_error("SYS_CONFIG_INVALID", "持久上传调度器依赖、容量或退避配置无效",
                            "upload.scheduler.create"));
    auto impl = std::make_shared<Impl>();
    impl->database = std::move(database);
    impl->config = config;
    impl->executor = std::move(executor);
    return Result<std::unique_ptr<PersistentUploadScheduler>>::success(
        std::make_unique<PersistentUploadScheduler>(ValidatedTag{}, std::move(impl)));
}

Result<storage::UploadJobEnqueueOutcome> PersistentUploadScheduler::enqueue(
    const storage::UploadJobRequest& request)
{
    auto result = impl_->database->enqueue_upload_job(request);
    if (result)
        impl_->condition.notify_all();
    return result;
}

Result<void> PersistentUploadScheduler::retry(const std::int64_t job_id)
{
    auto result = impl_->database->retry_upload_job(job_id, now_utc_ms());
    if (result)
        impl_->condition.notify_all();
    return result;
}

Result<void> PersistentUploadScheduler::start()
{
    {
        std::lock_guard lock{impl_->mutex};
        if (impl_->started)
            return Result<void>::failure(scheduler_error(
                "SYS_INVALID_STATE", "持久上传调度器已经启动", "upload.scheduler.start"));
        impl_->started = true;
        impl_->finished = false;
        impl_->metrics.state = PersistentUploadSchedulerState::running;
    }
    try
    {
        impl_->worker = std::thread{[state = impl_] { state->run(); }};
    }
    catch (...)
    {
        std::lock_guard lock{impl_->mutex};
        impl_->started = false;
        impl_->finished = true;
        impl_->metrics.state = PersistentUploadSchedulerState::stopped;
        return Result<void>::failure(scheduler_error(
            "SYS_INTERNAL_ERROR", "无法创建持久上传工作线程", "upload.scheduler.start"));
    }
    return Result<void>::success();
}

void PersistentUploadScheduler::request_stop() noexcept
{
    {
        std::lock_guard lock{impl_->mutex};
        if (!impl_->started || impl_->finished)
            return;
        impl_->metrics.state = PersistentUploadSchedulerState::stop_requested;
        impl_->stop_source.request_stop();
    }
    impl_->condition.notify_all();
}

Result<void> PersistentUploadScheduler::join(const std::chrono::steady_clock::time_point deadline)
{
    {
        std::unique_lock lock{impl_->mutex};
        if (!impl_->started)
            return Result<void>::success();
        if (!impl_->condition.wait_until(lock, deadline, [this] { return impl_->finished; }))
            return Result<void>::failure(scheduler_error("SYS_SHUTDOWN_TIMEOUT",
                                                         "持久上传工作线程未在截止时间内停止",
                                                         "upload.scheduler.join", true));
    }
    if (impl_->worker.joinable())
        impl_->worker.join();
    return Result<void>::success();
}

PersistentUploadSchedulerSnapshot PersistentUploadScheduler::snapshot() const noexcept
{
    std::lock_guard lock{impl_->mutex};
    return impl_->metrics;
}

std::string_view persistent_upload_scheduler_state_name(
    const PersistentUploadSchedulerState state) noexcept
{
    switch (state)
    {
    case PersistentUploadSchedulerState::stopped:
        return "Stopped";
    case PersistentUploadSchedulerState::running:
        return "Running";
    case PersistentUploadSchedulerState::stop_requested:
        return "StopRequested";
    }
    return "Stopped";
}

} // namespace paperbreak::uplink
