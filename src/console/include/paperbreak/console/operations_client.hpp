#pragma once

#include "paperbreak/common/result.hpp"
#include "paperbreak/common/threading.hpp"
#include "paperbreak/ipc/client.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace paperbreak::console
{

struct OperationsMetric final
{
    std::string name;
    std::string value;
    std::string unit;
    bool available{};
};

struct OperationsAlarm final
{
    std::uint64_t alarm_id{};
    std::string code;
    std::string severity;
    std::string source;
    std::string first_occurred_at;
    std::string last_occurred_at;
    bool active{};
    std::uint64_t occurrence_count{};
    std::string message;
    std::vector<std::pair<std::string, std::string>> details;
    bool acknowledged{};
};

struct OperationsLogRecord final
{
    std::uint64_t sequence{};
    std::string timestamp;
    std::uint64_t thread_id{};
    std::string thread_name;
    std::string category;
    std::string level;
    std::string message;
};

struct AlarmFilter final
{
    std::optional<bool> active{true};
    std::optional<std::string> minimum_severity;
    std::optional<std::string> source;
};

struct LogFilter final
{
    std::optional<std::string> category;
    std::optional<std::string> minimum_level;
    std::optional<std::string> thread_name;
};

struct OperationsSnapshot final
{
    ipc::ClientConnectionSnapshot connection;
    std::vector<OperationsMetric> metrics;
    std::vector<OperationsAlarm> alarms;
    std::vector<OperationsLogRecord> logs;
    bool metrics_stale{true};
    bool alarms_stale{true};
    bool logs_stale{true};
    bool alarms_truncated{};
    bool logs_truncated{};
    AlarmFilter alarm_filter;
    LogFilter log_filter;
    bool operation_pending{};
    std::string operation;
    std::optional<std::filesystem::path> exported_path;
    std::optional<Error> error;
};

using OperationsObserver = std::function<void(const OperationsSnapshot&)>;

class OperationsClient final
{
  public:
    explicit OperationsClient(OperationsObserver observer = {}, ipc::IpcClientOptions options = {},
                              ThreadRegistrationFactory register_thread = {});
    ~OperationsClient();
    OperationsClient(const OperationsClient&) = delete;
    OperationsClient& operator=(const OperationsClient&) = delete;

    [[nodiscard]] Result<void> start();
    void stop() noexcept;
    void refresh();
    [[nodiscard]] Result<void> query_alarms(AlarmFilter filter);
    [[nodiscard]] Result<void> query_logs(LogFilter filter);
    [[nodiscard]] Result<void> acknowledge(std::uint64_t alarm_id);
    [[nodiscard]] Result<void> export_diagnostics(std::filesystem::path destination);
    [[nodiscard]] Result<void> export_alarm_csv(std::filesystem::path destination);
    [[nodiscard]] const OperationsSnapshot& snapshot() const noexcept;

  private:
    class FileExporter;

    void connection_changed(const ipc::ClientConnectionSnapshot& connection);
    void push_received(std::uint64_t generation, const ipc::PushMessage& push);
    void refresh_metrics();
    void refresh_alarms();
    void refresh_logs();
    void metrics_completed(ipc::ClientRequestHandle handle, Result<ipc::ResponseMessage> result);
    void alarms_completed(ipc::ClientRequestHandle handle, Result<ipc::ResponseMessage> result);
    void logs_completed(ipc::ClientRequestHandle handle, Result<ipc::ResponseMessage> result);
    void operation_completed(ipc::ClientRequestHandle handle, Result<ipc::ResponseMessage> result,
                             std::filesystem::path destination);
    void export_completed(Result<std::filesystem::path> result);
    void notify() const noexcept;

    OperationsObserver observer_;
    OperationsSnapshot snapshot_;
    std::unique_ptr<ipc::IpcClient> client_;
    std::unique_ptr<FileExporter> exporter_;
    std::shared_ptr<std::atomic_bool> alive_;
    std::optional<ipc::ClientRequestHandle> metrics_request_;
    std::optional<ipc::ClientRequestHandle> alarms_request_;
    std::optional<ipc::ClientRequestHandle> logs_request_;
    std::optional<ipc::ClientRequestHandle> operation_request_;
};

} // namespace paperbreak::console
