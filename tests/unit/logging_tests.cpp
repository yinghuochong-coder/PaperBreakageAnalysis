#include "paperbreak/logging/logging.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <ranges>
#include <regex>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace
{

class TemporaryDirectory final
{
  public:
    TemporaryDirectory()
        : path_(std::filesystem::temp_directory_path() /
                ("paperbreak-m0-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
    {
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

std::string read_logs(const std::filesystem::path& directory)
{
    std::string content;
    for (const auto& entry : std::filesystem::directory_iterator{directory})
    {
        if (!entry.is_regular_file())
        {
            continue;
        }
        std::ifstream input{entry.path(), std::ios::binary};
        content.append(std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});
    }
    return content;
}

} // namespace

TEST(Logging, FiltersLevelsRedactsSecretsAndRotatesBySize)
{
    TemporaryDirectory temporary;
    paperbreak::logging::LoggingConfig config;
    config.directory = temporary.path();
    config.file_stem = "test";
    config.max_file_size_bytes = 512U;
    config.max_files_per_day = 4U;
    config.queue_capacity = 1024U;
    config.minimum_level = paperbreak::logging::Level::warning;

    auto created = paperbreak::logging::LoggingRuntime::create(config);
    ASSERT_TRUE(created) << created.error().message;
    auto runtime = std::move(created).value();
    auto registration = runtime->register_current_thread("service-main");
    ASSERT_TRUE(registration);

    ASSERT_TRUE(runtime->log(paperbreak::logging::Category::service,
                             paperbreak::logging::Level::info, "filtered-info-marker"));
    for (int index = 0; index < 100; ++index)
    {
        ASSERT_TRUE(runtime->log(
            paperbreak::logging::Category::audit, paperbreak::logging::Level::warning,
            "audit-marker password=do-not-write token=also-secret payload-padding-0123456789"));
    }
    ASSERT_TRUE(runtime->shutdown());

    std::size_t file_count = 0U;
    for (const auto& entry : std::filesystem::directory_iterator{temporary.path()})
    {
        if (entry.is_regular_file())
        {
            ++file_count;
            EXPECT_TRUE(std::regex_match(
                entry.path().filename().string(),
                std::regex{R"(^test-service-main-\d{4}-\d{2}-\d{2}\.log(?:\.\d+)?$)"}));
        }
    }
    EXPECT_GE(file_count, 2U);

    const auto content = read_logs(temporary.path());
    EXPECT_EQ(content.find("filtered-info-marker"), std::string::npos);
    EXPECT_EQ(content.find("do-not-write"), std::string::npos);
    EXPECT_EQ(content.find("also-secret"), std::string::npos);
    EXPECT_NE(content.find("password=***"), std::string::npos);
    EXPECT_NE(content.find("[audit]"), std::string::npos);
}

TEST(Logging, WritesTheSameRecordToConsoleAndFile)
{
    TemporaryDirectory temporary;
    paperbreak::logging::LoggingConfig config;
    config.directory = temporary.path();
    config.console_output_enabled = true;
    auto created = paperbreak::logging::LoggingRuntime::create(config);
    ASSERT_TRUE(created);
    auto runtime = std::move(created).value();
    auto registration = runtime->register_current_thread("service-main");
    ASSERT_TRUE(registration);

    testing::internal::CaptureStdout();
    ASSERT_TRUE(runtime->log(paperbreak::logging::Category::service,
                             paperbreak::logging::Level::info, "console-and-file-marker"));
    ASSERT_TRUE(runtime->shutdown());
    const auto console = testing::internal::GetCapturedStdout();

    EXPECT_NE(console.find("console-and-file-marker"), std::string::npos);
    EXPECT_NE(console.find("[service-main] [info] [service]"), std::string::npos);
    EXPECT_NE(read_logs(temporary.path()).find("console-and-file-marker"), std::string::npos);
}

TEST(Logging, ReportsDiagnosticErrorWhenDirectoryCannotBeCreated)
{
    TemporaryDirectory temporary;
    const auto blocker = temporary.path() / "not-a-directory";
    {
        std::ofstream output{blocker};
        output << "block";
    }

    paperbreak::logging::LoggingConfig config;
    config.directory = blocker / "logs";
    auto created = paperbreak::logging::LoggingRuntime::create(config);

    ASSERT_FALSE(created);
    EXPECT_EQ(created.error().business_code, "LOG_INITIALIZATION_FAILED");
    EXPECT_EQ(created.error().module, "logging");
    EXPECT_TRUE(created.error().native_code.has_value());
}

TEST(Logging, RedactsJsonAndKeyValueForms)
{
    const auto redacted = paperbreak::logging::redact_sensitive(
        R"({"token":"abc","private_key":"xyz"} password=hunter2)");
    EXPECT_EQ(redacted.find("abc"), std::string::npos);
    EXPECT_EQ(redacted.find("xyz"), std::string::npos);
    EXPECT_EQ(redacted.find("hunter2"), std::string::npos);
}

TEST(Logging, KeepsBoundedStructuredRecentRecordsAndFiltersTail)
{
    TemporaryDirectory temporary;
    paperbreak::logging::LoggingConfig config;
    config.directory = temporary.path();
    config.recent_record_capacity = 3U;
    auto created = paperbreak::logging::LoggingRuntime::create(config);
    ASSERT_TRUE(created);
    auto runtime = std::move(created).value();
    auto registration = runtime->register_current_thread("service-main");
    ASSERT_TRUE(registration);
    ASSERT_TRUE(runtime->log(paperbreak::logging::Category::service,
                             paperbreak::logging::Level::info, "one"));
    ASSERT_TRUE(runtime->log(paperbreak::logging::Category::camera,
                             paperbreak::logging::Level::warning, "token=secret two"));
    ASSERT_TRUE(runtime->log(paperbreak::logging::Category::service,
                             paperbreak::logging::Level::error, "three"));
    ASSERT_TRUE(runtime->log(paperbreak::logging::Category::service,
                             paperbreak::logging::Level::critical, "four"));
    ASSERT_TRUE(runtime->shutdown());

    auto all = runtime->tail({.limit = 10U});
    ASSERT_EQ(all.records.size(), 3U);
    EXPECT_TRUE(all.truncated);
    EXPECT_EQ(all.records.front().message.find("secret"), std::string::npos);
    auto filtered = runtime->tail({.categories = {paperbreak::logging::Category::camera},
                                   .minimum_level = paperbreak::logging::Level::warning,
                                   .limit = 10U});
    ASSERT_EQ(filtered.records.size(), 1U);
    EXPECT_EQ(filtered.records.front().category, paperbreak::logging::Category::camera);
    auto cursor =
        runtime->tail({.after_sequence = filtered.records.front().sequence, .limit = 10U});
    ASSERT_EQ(cursor.records.size(), 2U);
    EXPECT_FALSE(cursor.truncated);
    auto overwritten = runtime->tail({.after_sequence = 0U, .limit = 10U});
    EXPECT_TRUE(overwritten.truncated);
}

TEST(Logging, ConcurrentShutdownDoesNotRaceWithProducers)
{
    TemporaryDirectory temporary;
    paperbreak::logging::LoggingConfig config;
    config.directory = temporary.path();
    auto created = paperbreak::logging::LoggingRuntime::create(config);
    ASSERT_TRUE(created);
    auto runtime = std::move(created).value();
    std::jthread producer([&](std::stop_token) {
        auto registration = runtime->register_current_thread("test-producer");
        for (int index = 0; index < 1000; ++index)
        {
            static_cast<void>(runtime->log(paperbreak::logging::Category::performance,
                                           paperbreak::logging::Level::info, "sample"));
        }
    });
    ASSERT_TRUE(runtime->shutdown());
    producer.join();
    EXPECT_TRUE(runtime->shutdown());
}

TEST(Logging, SupportsConcurrentTailQueriesWhileProducing)
{
    TemporaryDirectory temporary;
    paperbreak::logging::LoggingConfig config;
    config.directory = temporary.path();
    config.recent_record_capacity = 64U;
    auto created = paperbreak::logging::LoggingRuntime::create(config);
    ASSERT_TRUE(created);
    auto runtime = std::move(created).value();
    std::atomic_bool failed{false};
    std::jthread producer([&](std::stop_token) {
        auto registration = runtime->register_current_thread("test-producer");
        for (int index = 0; index < 500; ++index)
        {
            if (!runtime->log(paperbreak::logging::Category::performance,
                              paperbreak::logging::Level::info, "sample"))
            {
                failed.store(true, std::memory_order_relaxed);
            }
        }
    });
    for (int index = 0; index < 500; ++index)
    {
        if (runtime->tail({.limit = 20U}).records.size() > 20U)
        {
            failed.store(true, std::memory_order_relaxed);
        }
    }
    producer.join();
    ASSERT_TRUE(runtime->shutdown());
    EXPECT_FALSE(failed.load(std::memory_order_relaxed));
}

TEST(Logging, ValidatesNamesSetsWindowsDescriptionAndRejectsDuplicates)
{
    TemporaryDirectory temporary;
    paperbreak::logging::LoggingConfig config;
    config.directory = temporary.path();
    auto created = paperbreak::logging::LoggingRuntime::create(config);
    ASSERT_TRUE(created);
    auto runtime = std::move(created).value();

    EXPECT_FALSE(runtime->register_current_thread("Invalid_Name"));
    auto registration = runtime->register_current_thread("service-main");
    ASSERT_TRUE(registration);
    EXPECT_FALSE(runtime->register_current_thread("service-main"));
#if defined(_WIN32)
    const HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentThreadId());
    ASSERT_NE(thread, nullptr);
    PWSTR description = nullptr;
    ASSERT_TRUE(SUCCEEDED(GetThreadDescription(thread, &description)));
    ASSERT_NE(description, nullptr);
    EXPECT_EQ(std::wstring_view{description}, L"service-main");
    LocalFree(description);
    CloseHandle(thread);
#endif
    ASSERT_TRUE(runtime->shutdown());
}

TEST(Logging, RoutesConcurrentThreadsAndSupportsThreadTailFilter)
{
    TemporaryDirectory temporary;
    paperbreak::logging::LoggingConfig config;
    config.directory = temporary.path();
    config.file_stem = "paperbreak-service";
    auto created = paperbreak::logging::LoggingRuntime::create(config);
    ASSERT_TRUE(created);
    auto runtime = std::move(created).value();
    std::atomic_int ready{};
    const auto produce = [&](const std::string_view name, const std::string_view marker) {
        auto registration = runtime->register_current_thread(name);
        ASSERT_TRUE(registration);
        ready.fetch_add(1);
        while (ready.load() != 2)
            std::this_thread::yield();
        ASSERT_TRUE(runtime->log(paperbreak::logging::Category::camera,
                                 paperbreak::logging::Level::info, marker));
    };
    std::jthread first([&] { produce("camera-forward-cam01", "cam01-only"); });
    std::jthread second([&] { produce("camera-forward-cam02", "cam02-only"); });
    first.join();
    second.join();
    ASSERT_TRUE(runtime->shutdown());

    std::string first_content;
    std::string second_content;
    for (const auto& entry : std::filesystem::directory_iterator{temporary.path()})
    {
        std::ifstream input{entry.path(), std::ios::binary};
        const std::string value{std::istreambuf_iterator<char>{input},
                                std::istreambuf_iterator<char>{}};
        const auto name = entry.path().filename().string();
        if (name.find("camera-forward-cam01") != std::string::npos)
            first_content += value;
        if (name.find("camera-forward-cam02") != std::string::npos)
            second_content += value;
    }
    EXPECT_NE(first_content.find("cam01-only"), std::string::npos);
    EXPECT_EQ(first_content.find("cam02-only"), std::string::npos);
    EXPECT_NE(second_content.find("cam02-only"), std::string::npos);
    EXPECT_EQ(second_content.find("cam01-only"), std::string::npos);
    const auto tail =
        runtime->tail({.thread_name = std::string{"camera-forward-cam01"}, .limit = 20U});
    ASSERT_FALSE(tail.records.empty());
    EXPECT_TRUE(std::ranges::all_of(tail.records, [](const auto& record) {
        return record.thread_name == "camera-forward-cam01";
    }));
}

TEST(Logging, UsesLocalRfc3339AndAppliesDynamicLevelAndStructuredLimit)
{
    TemporaryDirectory temporary;
    paperbreak::logging::LoggingConfig config;
    config.directory = temporary.path();
    auto created = paperbreak::logging::LoggingRuntime::create(config);
    ASSERT_TRUE(created);
    auto runtime = std::move(created).value();
    auto registration = runtime->register_current_thread("service-main");
    ASSERT_TRUE(registration);
    EXPECT_FALSE(runtime->enabled(paperbreak::logging::Level::debug));
    ASSERT_TRUE(runtime->log(paperbreak::logging::Category::service,
                             paperbreak::logging::Level::debug, "hidden-debug"));
    ASSERT_TRUE(runtime->set_minimum_level(paperbreak::logging::Level::debug));
    EXPECT_TRUE(runtime->enabled(paperbreak::logging::Level::debug));
    const std::array fields{paperbreak::logging::StructuredField{"cameraId", "CAM01"},
                            paperbreak::logging::StructuredField{"sequenceNumber", "42"}};
    ASSERT_TRUE(runtime->log({.category = paperbreak::logging::Category::camera,
                              .level = paperbreak::logging::Level::debug,
                              .operation = "frame.forward",
                              .result = "success",
                              .business_code = "OK",
                              .correlation_id = "corr-1",
                              .fields = fields}));
    ASSERT_TRUE(runtime->set_retention_days(7U));
    EXPECT_EQ(runtime->retention_days(), 7U);
    ASSERT_TRUE(runtime->shutdown());
    const auto tail = runtime->tail({.limit = 20U});
    EXPECT_TRUE(std::ranges::none_of(tail.records, [](const auto& record) {
        return record.message.find("hidden-debug") != std::string::npos;
    }));
    const auto found = std::ranges::find_if(tail.records, [](const auto& record) {
        return record.message.find("operation=frame.forward") != std::string::npos;
    });
    ASSERT_NE(found, tail.records.end());
    EXPECT_TRUE(std::regex_match(
        found->timestamp,
        std::regex{R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}[+-]\d{2}:\d{2}$)"}));
    std::array<paperbreak::logging::StructuredField, 17U> too_many{};
    EXPECT_FALSE(runtime->log({.fields = too_many}));
}

TEST(Logging, UsesEmergencyFileForUnregisteredAndThreadStateLimit)
{
    TemporaryDirectory temporary;
    paperbreak::logging::LoggingConfig config;
    config.directory = temporary.path();
    config.maximum_thread_file_states = 1U;
    auto created = paperbreak::logging::LoggingRuntime::create(config);
    ASSERT_TRUE(created);
    auto runtime = std::move(created).value();
    ASSERT_TRUE(runtime->log(paperbreak::logging::Category::service,
                             paperbreak::logging::Level::info, "unregistered-marker"));
    std::jthread named([&] {
        auto registration = runtime->register_current_thread("ipc-event");
        ASSERT_TRUE(registration);
        ASSERT_TRUE(runtime->log(paperbreak::logging::Category::ipc,
                                 paperbreak::logging::Level::error, "limited-marker"));
    });
    named.join();
    ASSERT_TRUE(runtime->shutdown());
    const auto content = read_logs(temporary.path());
    EXPECT_NE(content.find("unregistered-marker"), std::string::npos);
    EXPECT_NE(content.find("LOG_THREAD_FILE_LIMIT_REACHED"), std::string::npos);
}

TEST(Logging, AppendsSameNamedThreadAfterRuntimeRestart)
{
    TemporaryDirectory temporary;
    for (const std::string_view marker : {"first-instance", "second-instance"})
    {
        paperbreak::logging::LoggingConfig config;
        config.directory = temporary.path();
        auto created = paperbreak::logging::LoggingRuntime::create(config);
        ASSERT_TRUE(created);
        auto runtime = std::move(created).value();
        {
            auto registration = runtime->register_current_thread("service-main");
            ASSERT_TRUE(registration);
            ASSERT_TRUE(runtime->log(paperbreak::logging::Category::service,
                                     paperbreak::logging::Level::info, marker));
        }
        ASSERT_TRUE(runtime->shutdown());
    }
    const auto content = read_logs(temporary.path());
    EXPECT_NE(content.find("first-instance"), std::string::npos);
    EXPECT_NE(content.find("second-instance"), std::string::npos);
}

TEST(Logging, RejectsANameHeldByAnotherActiveThread)
{
    TemporaryDirectory temporary;
    paperbreak::logging::LoggingConfig config;
    config.directory = temporary.path();
    auto created = paperbreak::logging::LoggingRuntime::create(config);
    ASSERT_TRUE(created);
    auto runtime = std::move(created).value();
    std::atomic_bool registered{false};
    std::atomic_bool release{false};
    std::jthread owner([&] {
        auto registration = runtime->register_current_thread("event-processing");
        ASSERT_TRUE(registration);
        registered.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire))
            std::this_thread::yield();
    });
    while (!registered.load(std::memory_order_acquire))
        std::this_thread::yield();
    EXPECT_FALSE(runtime->register_current_thread("event-processing"));
    release.store(true, std::memory_order_release);
    owner.join();
    ASSERT_TRUE(runtime->shutdown());
    EXPECT_NE(read_logs(temporary.path()).find("reason=duplicate-name"), std::string::npos);
}

TEST(Logging, RetentionOnlyDeletesRecognizedExpiredThreadFiles)
{
    TemporaryDirectory temporary;
    const auto expired = temporary.path() / "paperbreak-service-service-main-2000-01-01.log";
    const auto legacy = temporary.path() / "paperbreak-service-2000-01-01.log";
    const auto unrelated = temporary.path() / "operator-notes-2000-01-01.log";
    for (const auto& path : {expired, legacy, unrelated})
    {
        std::ofstream output{path};
        output << "keep-or-remove";
    }

    paperbreak::logging::LoggingConfig config;
    config.directory = temporary.path();
    config.file_stem = "paperbreak-service";
    config.retention_days = 1U;
    auto created = paperbreak::logging::LoggingRuntime::create(config);
    ASSERT_TRUE(created);
    auto runtime = std::move(created).value();
    auto registration = runtime->register_current_thread("service-main");
    ASSERT_TRUE(registration);
    ASSERT_TRUE(runtime->shutdown());

    EXPECT_FALSE(std::filesystem::exists(expired));
    EXPECT_TRUE(std::filesystem::exists(legacy));
    EXPECT_TRUE(std::filesystem::exists(unrelated));
}

TEST(Logging, CountsAsyncQueueOverwriteWithoutBlockingProducer)
{
    TemporaryDirectory temporary;
    paperbreak::logging::LoggingConfig config;
    config.directory = temporary.path();
    config.queue_capacity = 1U;
    config.max_file_size_bytes = 1024U * 1024U * 64U;
    auto created = paperbreak::logging::LoggingRuntime::create(config);
    ASSERT_TRUE(created);
    auto runtime = std::move(created).value();
    auto registration = runtime->register_current_thread("camera-acquisition-cam01");
    ASSERT_TRUE(registration);
    for (int index = 0; index < 20000; ++index)
    {
        ASSERT_TRUE(runtime->log(paperbreak::logging::Category::performance,
                                 paperbreak::logging::Level::info,
                                 "frame-marker-with-padding-0123456789"));
    }
    ASSERT_TRUE(runtime->shutdown());
    EXPECT_GT(runtime->overrun_count(), 0U);
}
