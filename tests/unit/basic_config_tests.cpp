#include "paperbreak/config/basic_config.hpp"
#include "paperbreak/config/config_repository.hpp"
#include "paperbreak/platform/atomic_file.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace
{

class TemporaryDirectory final
{
  public:
    TemporaryDirectory()
    {
        static std::atomic_uint64_t sequence{0};
        path_ = std::filesystem::temp_directory_path() /
                ("paperbreak-config-test-" +
                 std::to_string(sequence.fetch_add(1U, std::memory_order_relaxed)));
        std::filesystem::create_directories(path_);
    }
    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] std::filesystem::path write(std::string_view name,
                                              std::string_view contents) const
    {
        std::u8string converted;
        converted.reserve(name.size());
        for (const unsigned char byte : name)
            converted.push_back(static_cast<char8_t>(byte));
        const auto file = path_ / std::filesystem::path{converted};
        std::ofstream stream{file, std::ios::binary};
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        return file;
    }
  private:
    std::filesystem::path path_;
};

std::string valid_config()
{
    const auto path = std::filesystem::path{PAPERBREAK_TEST_SOURCE_DIR} / "data" /
                      "basic-config-valid.json";
    std::ifstream stream{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

std::string replace_once(std::string value, const std::string_view from,
                         const std::string_view to)
{
    const auto position = value.find(from);
    EXPECT_NE(position, std::string::npos);
    if (position != std::string::npos)
        value.replace(position, from.size(), to);
    return value;
}

class RecordingAudit final : public paperbreak::config::IConfigAuditSink
{
  public:
    paperbreak::Result<void> record(
        const paperbreak::config::ConfigAuditRecord& record) override
    {
        records.push_back(record);
        if (fail)
            return paperbreak::Result<void>::failure(paperbreak::make_error(
                "LOG_WRITE_FAILED", paperbreak::Severity::error, "injected", "test",
                "test.audit"));
        return paperbreak::Result<void>::success();
    }
    bool fail{};
    std::vector<paperbreak::config::ConfigAuditRecord> records;
};

class RecordingApplier final : public paperbreak::config::IConfigApplier
{
  public:
    [[nodiscard]] std::string_view name() const noexcept override { return "test-applier"; }
    paperbreak::Result<void> prepare(const paperbreak::config::EdgeConfig&,
                                     const paperbreak::config::EdgeConfig&,
                                     const std::vector<std::string>&) override
    {
        calls.emplace_back("prepare");
        return paperbreak::Result<void>::success();
    }
    paperbreak::Result<void> apply_and_readback(
        const paperbreak::config::EdgeConfig&) override
    {
        calls.emplace_back("apply");
        if (fail_apply)
            return paperbreak::Result<void>::failure(paperbreak::make_error(
                "CAMERA_CONFIG_FAILED", paperbreak::Severity::error, "injected", "test",
                "test.apply"));
        return paperbreak::Result<void>::success();
    }
    paperbreak::Result<void> commit(const paperbreak::config::EdgeConfig&) override
    {
        calls.emplace_back("commit");
        if (fail_commit)
            return paperbreak::Result<void>::failure(paperbreak::make_error(
                "CAMERA_CONFIG_FAILED", paperbreak::Severity::error, "injected", "test",
                "test.commit"));
        return paperbreak::Result<void>::success();
    }
    paperbreak::Result<void> rollback(const paperbreak::config::EdgeConfig&) noexcept override
    {
        calls.emplace_back("rollback");
        return paperbreak::Result<void>::success();
    }
    bool fail_apply{};
    bool fail_commit{};
    std::vector<std::string> calls;
};

class FailingAtomicFileSystem final : public paperbreak::platform::IAtomicFileSystem
{
  public:
    paperbreak::Result<std::string> read_bounded(const std::filesystem::path& path,
                                                 const std::size_t maximum) override
    { return inner.read_bounded(path, maximum); }
    paperbreak::Result<void> create_directories(const std::filesystem::path& path) override
    { return inner.create_directories(path); }
    paperbreak::Result<std::vector<std::filesystem::path>> list_regular_files(
        const std::filesystem::path& path) override
    { return inner.list_regular_files(path); }
    paperbreak::Result<void> remove_file(const std::filesystem::path& path) override
    { return inner.remove_file(path); }
    paperbreak::Result<void> replace_atomically(
        const std::filesystem::path& path, const std::string_view contents,
        const std::optional<std::filesystem::path>& backup) override
    {
        ++replace_calls;
        if (fail_on_replace != 0U && replace_calls == fail_on_replace)
            return paperbreak::Result<void>::failure(paperbreak::make_error(
                "SYS_CONFIG_PERSIST_FAILED", paperbreak::Severity::critical, "injected", "test",
                "test.replace"));
        return inner.replace_atomically(path, contents, backup);
    }
    paperbreak::platform::WindowsAtomicFileSystem inner;
    std::size_t replace_calls{};
    std::size_t fail_on_replace{};
};

} // namespace

TEST(BasicConfig, AcceptsCompleteVersionOneAtUnicodeAndSpacePath)
{
    const TemporaryDirectory directory;
    const auto path = directory.write("纸机 配置.json", valid_config());
    const auto result = paperbreak::config::validate_basic_config(path);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result.value().schema_version, 1U);
    EXPECT_EQ(result.value().config_revision, 1U);
}

TEST(BasicConfig, RejectsUnknownSensitiveMalformedAndUnsupportedSchema)
{
    const TemporaryDirectory directory;
    const auto unknown = directory.write(
        "unknown.json", replace_once(valid_config(), "\"health\": {", "\"extra\": 1, \"health\": {"));
    const auto sensitive = directory.write(
        "secret.json", replace_once(valid_config(), "\"serverUrl\": \"\"", "\"serverUrl\": \"\", \"token\": \"raw\""));
    const auto malformed = directory.write("truncated.json", R"({"configSchemaVersion":1)");
    const auto future = directory.write(
        "future.json", replace_once(valid_config(), "\"configSchemaVersion\": 1", "\"configSchemaVersion\": 2"));
    EXPECT_FALSE(paperbreak::config::validate_basic_config(unknown));
    EXPECT_FALSE(paperbreak::config::validate_basic_config(sensitive));
    EXPECT_FALSE(paperbreak::config::validate_basic_config(malformed));
    const auto future_result = paperbreak::config::validate_basic_config(future);
    ASSERT_FALSE(future_result);
    EXPECT_EQ(future_result.error().business_code, "SYS_CONFIG_SCHEMA_UNSUPPORTED");
}

TEST(BasicConfig, RejectsCrossFieldAndPathViolations)
{
    const TemporaryDirectory directory;
    const auto event = directory.write(
        "event.json", replace_once(valid_config(), "\"maxEventSeconds\": 60", "\"maxEventSeconds\": 15"));
    const auto watermarks = directory.write(
        "watermarks.json", replace_once(valid_config(), "\"criticalFreeSpaceGiB\": 100", "\"criticalFreeSpaceGiB\": 250"));
    const auto path = directory.write(
        "path.json", replace_once(valid_config(), "\"eventRoot\": \"数据/事件 文件\"", "\"eventRoot\": \"../escape\""));
    EXPECT_FALSE(paperbreak::config::validate_basic_config(event));
    EXPECT_FALSE(paperbreak::config::validate_basic_config(watermarks));
    EXPECT_FALSE(paperbreak::config::validate_basic_config(path));
}

TEST(BasicConfig, RejectsEmptyMissingDirectoryAndOversizedFiles)
{
    const TemporaryDirectory directory;
    EXPECT_FALSE(paperbreak::config::validate_basic_config(directory.write("empty.json", "")));
    EXPECT_FALSE(paperbreak::config::validate_basic_config(directory.path() / "missing.json"));
    EXPECT_FALSE(paperbreak::config::validate_basic_config(directory.path()));
    const std::string oversized(paperbreak::config::config_max_bytes + 1U, 'x');
    EXPECT_FALSE(paperbreak::config::validate_basic_config(directory.write("large.json", oversized)));
}

TEST(ConfigRepository, CommitsHotChangesAndIsIdempotent)
{
    const TemporaryDirectory directory;
    const auto path = directory.write("config.json", valid_config());
    paperbreak::platform::WindowsAtomicFileSystem files;
    RecordingAudit audit;
    RecordingApplier applier;
    paperbreak::config::ConfigRepository repository{path, files, audit, {&applier}};
    ASSERT_TRUE(repository.load());
    const auto candidate = replace_once(valid_config(), "\"fps\": 3.0", "\"fps\": 4.0");
    const auto updated = repository.update(candidate, 1U,
        {.source = paperbreak::config::ConfigChangeSource::local_file,
         .actor = "operator", .correlation_id = "req-1"});
    ASSERT_TRUE(updated) << updated.error().message;
    EXPECT_EQ(updated.value().stored_config_revision, 2U);
    EXPECT_EQ(updated.value().effective_config_revision, 2U);
    EXPECT_TRUE(updated.value().pending_restart_paths.empty());
    EXPECT_EQ(audit.records.size(), 1U);
    EXPECT_EQ(applier.calls, (std::vector<std::string>{"prepare", "apply", "commit"}));

    auto current_json = paperbreak::config::serialize_config(*updated.value().stored);
    const auto idempotent = repository.update(current_json, 2U,
        {.source = paperbreak::config::ConfigChangeSource::local_file,
         .actor = "operator", .correlation_id = "req-2"});
    ASSERT_TRUE(idempotent);
    EXPECT_EQ(idempotent.value().stored_config_revision, 2U);
    EXPECT_EQ(audit.records.size(), 1U);
}

TEST(ConfigRepository, ReportsVersionConflictAndPendingRestartWithoutApplyingStoredIdentity)
{
    const TemporaryDirectory directory;
    const auto path = directory.write("config.json", valid_config());
    paperbreak::platform::WindowsAtomicFileSystem files;
    RecordingAudit audit;
    paperbreak::config::ConfigRepository repository{path, files, audit};
    ASSERT_TRUE(repository.load());
    const auto candidate = replace_once(valid_config(), "EDGE-TEST", "EDGE-NEW");
    const auto conflict = repository.update(candidate, 9U, {});
    ASSERT_FALSE(conflict);
    EXPECT_EQ(conflict.error().business_code, "SYS_CONFIG_VERSION_CONFLICT");
    const auto updated = repository.update(candidate, 1U, {});
    ASSERT_TRUE(updated);
    EXPECT_EQ(updated.value().stored->system.machine_id, "EDGE-NEW");
    EXPECT_EQ(updated.value().effective->system.machine_id, "EDGE-TEST");
    EXPECT_EQ(updated.value().effective_config_revision, 1U);
    EXPECT_NE(std::ranges::find(updated.value().pending_restart_paths, "/system"),
              updated.value().pending_restart_paths.end());
}

TEST(ConfigRepository, RollsBackAppliedComponentsOnApplyAndPersistenceFailures)
{
    const TemporaryDirectory directory;
    const auto path = directory.write("config.json", valid_config());
    FailingAtomicFileSystem files;
    RecordingAudit audit;
    RecordingApplier applier;
    paperbreak::config::ConfigRepository repository{path, files, audit, {&applier}};
    ASSERT_TRUE(repository.load());
    const auto candidate = replace_once(valid_config(), "\"fps\": 3.0", "\"fps\": 4.0");

    applier.fail_apply = true;
    const auto apply_failure = repository.update(candidate, 1U, {});
    ASSERT_FALSE(apply_failure);
    EXPECT_EQ(apply_failure.error().business_code, "SYS_CONFIG_APPLY_FAILED");
    EXPECT_EQ(applier.calls, (std::vector<std::string>{"prepare", "apply", "rollback"}));
    applier.fail_apply = false;
    applier.calls.clear();
    files.fail_on_replace = 2U;
    const auto write_failure = repository.update(candidate, 1U, {});
    ASSERT_FALSE(write_failure);
    EXPECT_EQ(write_failure.error().business_code, "SYS_CONFIG_PERSIST_FAILED");
    EXPECT_EQ(applier.calls, (std::vector<std::string>{"prepare", "apply", "rollback"}));
    ASSERT_TRUE(repository.snapshot());
    EXPECT_EQ(repository.snapshot().value().stored_config_revision, 1U);
}

TEST(ConfigRepository, RestoresDiskAndRuntimeWhenCommitFails)
{
    const TemporaryDirectory directory;
    const auto path = directory.write("config.json", valid_config());
    paperbreak::platform::WindowsAtomicFileSystem files;
    RecordingAudit audit;
    RecordingApplier applier;
    applier.fail_commit = true;
    paperbreak::config::ConfigRepository repository{path, files, audit, {&applier}};
    ASSERT_TRUE(repository.load());
    const auto result = repository.update(
        replace_once(valid_config(), "\"fps\": 3.0", "\"fps\": 4.0"), 1U, {});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().business_code, "SYS_CONFIG_APPLY_FAILED");
    EXPECT_EQ(applier.calls,
              (std::vector<std::string>{"prepare", "apply", "commit", "rollback"}));
    ASSERT_TRUE(repository.snapshot());
    EXPECT_EQ(repository.snapshot().value().stored_config_revision, 1U);
    const auto persisted = files.read_bounded(path, paperbreak::config::config_max_bytes);
    ASSERT_TRUE(persisted);
    EXPECT_EQ(paperbreak::config::parse_config(persisted.value(), directory.path()).value().
                  config_revision,
              1U);
}

TEST(ConfigRepository, AuditValuesAreBoundedAndReferencesAreRedacted)
{
    const TemporaryDirectory directory;
    const auto path = directory.write("config.json", valid_config());
    paperbreak::platform::WindowsAtomicFileSystem files;
    RecordingAudit audit;
    paperbreak::config::ConfigRepository repository{path, files, audit};
    ASSERT_TRUE(repository.load());
    const auto candidate = replace_once(valid_config(), "\"credentialReference\": \"\"",
                                        "\"credentialReference\": \"vault-entry-42\"");
    ASSERT_TRUE(repository.update(candidate, 1U, {}));
    ASSERT_EQ(audit.records.size(), 1U);
    ASSERT_FALSE(audit.records.front().redacted_changes.empty());
    for (const auto& change : audit.records.front().redacted_changes)
    {
        EXPECT_EQ(change.previous_value.find("vault-entry-42"), std::string::npos);
        EXPECT_EQ(change.candidate_value.find("vault-entry-42"), std::string::npos);
        EXPECT_LE(change.previous_value.size(), 4110U);
        EXPECT_LE(change.candidate_value.size(), 4110U);
    }
}

TEST(ConfigRepository, RecoversNewestValidHistoryAndIgnoresTemporaryResidue)
{
    const TemporaryDirectory directory;
    const auto path = directory.write("config.json", "{truncated");
    const auto history = directory.path() / "config.json.history";
    std::filesystem::create_directories(history);
    const auto valid = replace_once(valid_config(), "\"configRevision\": 1", "\"configRevision\": 7");
    std::ofstream{history / "00000000000000000007.json", std::ios::binary} << valid;
    std::ofstream{directory.path() / "config.json.paperbreak.tmp.1.1", std::ios::binary}
        << replace_once(valid, "\"configRevision\": 7", "\"configRevision\": 99");
    paperbreak::platform::WindowsAtomicFileSystem files;
    RecordingAudit audit;
    paperbreak::config::ConfigRepository repository{path, files, audit};
    const auto loaded = repository.load();
    ASSERT_TRUE(loaded) << loaded.error().message;
    EXPECT_TRUE(loaded.value().recovered_from_history);
    EXPECT_EQ(loaded.value().stored_config_revision, 7U);
    EXPECT_EQ(audit.records.front().source,
              paperbreak::config::ConfigChangeSource::startup_recovery);
}

TEST(ConfigRepository, AuditFailurePreventsModification)
{
    const TemporaryDirectory directory;
    const auto path = directory.write("config.json", valid_config());
    paperbreak::platform::WindowsAtomicFileSystem files;
    RecordingAudit audit;
    paperbreak::config::ConfigRepository repository{path, files, audit};
    ASSERT_TRUE(repository.load());
    audit.fail = true;
    const auto result = repository.update(
        replace_once(valid_config(), "\"fps\": 3.0", "\"fps\": 4.0"), 1U, {});
    ASSERT_FALSE(result);
    EXPECT_EQ(repository.snapshot().value().stored_config_revision, 1U);
}
