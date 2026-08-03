#pragma once

#include "paperbreak/common/result.hpp"
#include "paperbreak/ipc/client.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace paperbreak::console
{

struct ServiceStatusSummary final
{
    std::string service_state;
    std::string machine_id;
    std::string service_timestamp;
    bool accepting_writes{};
    std::uint64_t generation{};
};

struct VersionSummary final
{
    std::string application_version;
    std::string git_commit;
    std::uint64_t generation{};
};

struct SystemMetricsSummary final
{
    std::string sampled_at;
    std::optional<double> process_cpu_percent;
    std::optional<double> system_memory_used_percent;
    std::optional<double> event_disk_free_gib;
    std::uint64_t generation{};
};

struct ActiveAlarmSummary final
{
    std::uint64_t alarm_id{};
    std::string severity;
    std::string source;
    std::string last_occurred_at;
    std::string message;
    bool acknowledged{};
};

struct AlarmOverviewSummary final
{
    std::size_t active_count{};
    bool count_truncated{};
    std::vector<ActiveAlarmSummary> recent;
    std::uint64_t generation{};
};

struct ClientStateSnapshot final
{
    ipc::ClientConnectionSnapshot connection;
    std::optional<ServiceStatusSummary> service_status;
    bool service_status_stale{true};
    std::optional<Error> synchronization_error;
    std::optional<VersionSummary> version;
    bool version_stale{true};
    std::optional<Error> version_error;
    std::optional<SystemMetricsSummary> metrics;
    bool metrics_stale{true};
    std::optional<Error> metrics_error;
    std::optional<AlarmOverviewSummary> alarms;
    bool alarms_stale{true};
    std::optional<Error> alarms_error;
};

using ClientStateObserver = std::function<void(const ClientStateSnapshot&)>;

class ClientStateStore final
{
  public:
    // The store is thread-confined to the Qt event-loop thread that constructs it.
    explicit ClientStateStore(ClientStateObserver observer = {},
                              ipc::IpcClientOptions options = {});
    ~ClientStateStore();

    ClientStateStore(const ClientStateStore&) = delete;
    ClientStateStore& operator=(const ClientStateStore&) = delete;
    ClientStateStore(ClientStateStore&&) = delete;
    ClientStateStore& operator=(ClientStateStore&&) = delete;

    [[nodiscard]] Result<void> start();
    void stop() noexcept;
    void refresh_dynamic();
    [[nodiscard]] const ClientStateSnapshot& snapshot() const noexcept;

  private:
    void connection_changed(const ipc::ClientConnectionSnapshot& connection);
    void push_received(std::uint64_t generation, const ipc::PushMessage& push);
    void synchronize_status(std::uint64_t generation);
    void synchronize_version(std::uint64_t generation);
    void synchronize_metrics(std::uint64_t generation);
    void synchronize_alarms(std::uint64_t generation);
    void status_completed(ipc::ClientRequestHandle handle, Result<ipc::ResponseMessage> result);
    void version_completed(ipc::ClientRequestHandle handle, Result<ipc::ResponseMessage> result);
    void metrics_completed(ipc::ClientRequestHandle handle, Result<ipc::ResponseMessage> result);
    void alarms_completed(ipc::ClientRequestHandle handle, Result<ipc::ResponseMessage> result);
    void notify() const noexcept;

    ClientStateObserver observer_;
    ClientStateSnapshot snapshot_;
    std::unique_ptr<ipc::IpcClient> client_;
    std::optional<ipc::ClientRequestHandle> status_request_;
    std::optional<ipc::ClientRequestHandle> version_request_;
    std::optional<ipc::ClientRequestHandle> metrics_request_;
    std::optional<ipc::ClientRequestHandle> alarms_request_;
    bool alarm_push_refresh_pending_{};
};

} // namespace paperbreak::console
