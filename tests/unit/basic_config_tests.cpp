#include "paperbreak/config/basic_config.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace
{

class TemporaryDirectory final
{
  public:
    TemporaryDirectory()
    {
        static std::atomic_uint64_t sequence{0};
        const auto identifier = sequence.fetch_add(1U, std::memory_order_relaxed);
        path_ = std::filesystem::temp_directory_path() /
                ("paperbreak-config-test-" + std::to_string(identifier));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

    [[nodiscard]] std::filesystem::path write(std::string_view name,
                                              std::string_view contents) const
    {
        const auto file = path_ / std::filesystem::path{name};
        std::ofstream stream{file, std::ios::binary};
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        stream.close();
        return file;
    }

  private:
    std::filesystem::path path_;
};

} // namespace

TEST(BasicConfig, AcceptsVersionOneAtUnicodeAndSpacePath)
{
    const TemporaryDirectory directory;
    const auto path = directory.write("纸机 配置.json", R"({"schemaVersion":1,"machine":"测试"})");

    const auto result = paperbreak::config::validate_basic_config(path);

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().schema_version, 1U);
    EXPECT_GT(result.value().file_size_bytes, 0U);
}

TEST(BasicConfig, RejectsMissingAndDirectoryPaths)
{
    const TemporaryDirectory directory;

    const auto missing =
        paperbreak::config::validate_basic_config(directory.path() / "missing.json");
    const auto not_file = paperbreak::config::validate_basic_config(directory.path());

    ASSERT_FALSE(missing);
    ASSERT_FALSE(not_file);
    EXPECT_EQ(missing.error().business_code, "SYS_CONFIG_INVALID");
    EXPECT_EQ(not_file.error().business_code, "SYS_CONFIG_INVALID");
}

TEST(BasicConfig, RejectsEmptyMalformedAndNonObjectJson)
{
    const TemporaryDirectory directory;
    const auto empty_path = directory.write("empty.json", "");
    const auto malformed_path = directory.write("truncated.json", R"({"schemaVersion":1)");
    const auto array_path = directory.write("array.json", R"([{"schemaVersion":1}])");

    EXPECT_FALSE(paperbreak::config::validate_basic_config(empty_path));
    EXPECT_FALSE(paperbreak::config::validate_basic_config(malformed_path));
    EXPECT_FALSE(paperbreak::config::validate_basic_config(array_path));
}

TEST(BasicConfig, RejectsMissingSignedFloatingAndUnsupportedSchemaVersions)
{
    const TemporaryDirectory directory;
    const auto missing_path = directory.write("missing-version.json", R"({"machine":"EDGE"})");
    const auto signed_path = directory.write("negative-version.json", R"({"schemaVersion":-1})");
    const auto floating_path = directory.write("float-version.json", R"({"schemaVersion":1.0})");
    const auto unsupported_path = directory.write("future-version.json", R"({"schemaVersion":2})");

    EXPECT_FALSE(paperbreak::config::validate_basic_config(missing_path));
    EXPECT_FALSE(paperbreak::config::validate_basic_config(signed_path));
    EXPECT_FALSE(paperbreak::config::validate_basic_config(floating_path));
    const auto unsupported = paperbreak::config::validate_basic_config(unsupported_path);
    ASSERT_FALSE(unsupported);
    EXPECT_EQ(unsupported.error().business_code, "SYS_CONFIG_SCHEMA_UNSUPPORTED");
}

TEST(BasicConfig, RejectsFilesAboveTheBoundBeforeParsing)
{
    const TemporaryDirectory directory;
    const std::string oversized(paperbreak::config::basic_config_max_bytes + 1U, 'x');
    const auto path = directory.write("oversized.json", oversized);

    const auto result = paperbreak::config::validate_basic_config(path);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().business_code, "SYS_CONFIG_INVALID");
}
