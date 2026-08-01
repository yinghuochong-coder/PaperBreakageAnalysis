#include "paperbreak/config/basic_config.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <limits>
#include <string>
#include <system_error>

namespace paperbreak::config
{
namespace
{

Error invalid_config(std::string message, std::string operation, std::string reason)
{
    Error error = make_error("SYS_CONFIG_INVALID", Severity::error, std::move(message), "config",
                             std::move(operation));
    error.details.push_back({"reason", std::move(reason)});
    return error;
}

} // namespace

Result<BasicConfigInfo> validate_basic_config(const std::filesystem::path& path,
                                              const std::size_t maximum_bytes) noexcept
{
    if (path.empty() || maximum_bytes == 0U)
    {
        return Result<BasicConfigInfo>::failure(
            invalid_config("配置路径为空或读取上限无效", "config.validateBasic", "invalid-input"));
    }

    try
    {
        std::error_code file_error;
        const bool regular_file = std::filesystem::is_regular_file(path, file_error);
        if (file_error || !regular_file)
        {
            return Result<BasicConfigInfo>::failure(invalid_config(
                "配置文件不存在或不是普通文件", "config.inspectFile", "not-regular-file"));
        }

        const std::uintmax_t file_size = std::filesystem::file_size(path, file_error);
        if (file_error)
        {
            return Result<BasicConfigInfo>::failure(
                invalid_config("无法读取配置文件大小", "config.inspectFile", "file-size-failed"));
        }
        if (file_size == 0U || file_size > maximum_bytes ||
            file_size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()))
        {
            Error error = invalid_config("配置文件为空或超过大小上限", "config.inspectFile",
                                         "invalid-file-size");
            error.details.push_back({"maximumBytes", std::to_string(maximum_bytes)});
            error.details.push_back({"actualBytes", std::to_string(file_size)});
            return Result<BasicConfigInfo>::failure(std::move(error));
        }

        std::ifstream stream{path, std::ios::binary};
        if (!stream)
        {
            return Result<BasicConfigInfo>::failure(
                invalid_config("无法打开配置文件", "config.openFile", "open-failed"));
        }

        std::string contents(static_cast<std::size_t>(file_size), '\0');
        stream.read(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (stream.gcount() != static_cast<std::streamsize>(contents.size()) || stream.bad())
        {
            return Result<BasicConfigInfo>::failure(
                invalid_config("配置文件读取不完整", "config.readFile", "short-read"));
        }

        const nlohmann::json document =
            nlohmann::json::parse(contents.cbegin(), contents.cend(), nullptr, false, true);
        if (document.is_discarded())
        {
            return Result<BasicConfigInfo>::failure(invalid_config(
                "配置文件不是合法的 UTF-8 JSON", "config.parseJson", "invalid-json"));
        }
        if (!document.is_object())
        {
            return Result<BasicConfigInfo>::failure(invalid_config(
                "配置 JSON 根节点必须是对象", "config.validateRoot", "root-not-object"));
        }

        const auto schema = document.find("schemaVersion");
        if (schema == document.end() || !schema->is_number_unsigned())
        {
            return Result<BasicConfigInfo>::failure(invalid_config("schemaVersion 必须是无符号整数",
                                                                   "config.validateSchemaVersion",
                                                                   "invalid-schema-version"));
        }

        const std::uint64_t version = schema->get<std::uint64_t>();
        if (version != basic_config_schema_version)
        {
            Error error =
                make_error("SYS_CONFIG_SCHEMA_UNSUPPORTED", Severity::error,
                           "不支持该配置 schemaVersion", "config", "config.validateSchemaVersion");
            error.details.push_back({"schemaVersion", std::to_string(version)});
            error.details.push_back(
                {"supportedVersion", std::to_string(basic_config_schema_version)});
            return Result<BasicConfigInfo>::failure(std::move(error));
        }

        return Result<BasicConfigInfo>::success(
            BasicConfigInfo{.schema_version = static_cast<std::uint32_t>(version),
                            .file_size_bytes = static_cast<std::size_t>(file_size)});
    }
    catch (const std::exception&)
    {
        return Result<BasicConfigInfo>::failure(invalid_config(
            "基础配置校验发生未预期错误", "config.validateBasic", "unexpected-exception"));
    }
    catch (...)
    {
        return Result<BasicConfigInfo>::failure(invalid_config(
            "基础配置校验发生未知错误", "config.validateBasic", "unknown-exception"));
    }
}

} // namespace paperbreak::config
