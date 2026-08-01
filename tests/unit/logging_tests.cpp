#include "paperbreak/logging/logging.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>
#include <thread>

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
            EXPECT_TRUE(std::regex_match(entry.path().filename().string(),
                                         std::regex{R"(^test-\d{4}-\d{2}-\d{2}\.log(?:\.\d+)?$)"}));
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
