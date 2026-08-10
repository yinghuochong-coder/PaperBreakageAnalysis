#include "paperbreak/storage/event_inspector.hpp"
#include "paperbreak/storage/event_store.hpp"
#include "paperbreak/storage/metadata_database.hpp"
#include "paperbreak/storage/nvme_cache.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
using namespace std::chrono_literals;
using namespace paperbreak;
using namespace paperbreak::camera;
using namespace paperbreak::event;
using namespace paperbreak::storage;

std::uint32_t test_little_u32(const std::span<const std::byte> bytes, const std::size_t offset)
{
    std::uint32_t value{};
    for (std::size_t index = 0U; index < 4U; ++index)
        value |= std::to_integer<std::uint32_t>(bytes[offset + index]) << (index * 8U);
    return value;
}

std::uint64_t test_little_u64(const std::span<const std::byte> bytes, const std::size_t offset)
{
    std::uint64_t value{};
    for (std::size_t index = 0U; index < 8U; ++index)
        value |= std::to_integer<std::uint64_t>(bytes[offset + index]) << (index * 8U);
    return value;
}

void test_put_u32(const std::span<std::byte> bytes, const std::size_t offset,
                  const std::uint32_t value)
{
    for (std::size_t index = 0U; index < 4U; ++index)
        bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
}

class TemporaryDirectory final
{
  public:
    explicit TemporaryDirectory(const std::string& suffix)
    {
        static std::atomic_uint64_t sequence{};
        path_ = std::filesystem::temp_directory_path() /
                (L"PaperBreak-M5-06-中文-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                 std::to_wstring(sequence.fetch_add(1U)) + L"-" +
                 std::filesystem::path{suffix}.wstring());
        std::error_code error;
        std::filesystem::create_directories(path_, error);
        if (error)
            throw std::runtime_error{"failed to create test directory"};
    }

    ~TemporaryDirectory()
    {
        if (path_.filename().wstring().starts_with(L"PaperBreak-M5-06-"))
        {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

FrameView frame(const std::string& camera_id, const std::uint64_t sequence,
                const std::chrono::milliseconds offset)
{
    auto buffer = std::make_shared<FrameBuffer>(16U);
    for (std::size_t index = 0U; index < 16U; ++index)
        buffer->writable_bytes()[index] = static_cast<std::byte>(sequence + index);
    EXPECT_TRUE(buffer->set_size(16U));
    const auto wall_base = WallClockTime{std::chrono::sys_days{std::chrono::year{2026} / 8 / 4}};
    auto result = make_frame_view({.camera_id = camera_id,
                                   .camera_frame_number = 100U + sequence,
                                   .sequence_number = sequence,
                                   .received_monotonic_time = MonotonicTime{offset},
                                   .received_wall_clock_time = wall_base + offset,
                                   .geometry = {.width = 4U, .height = 4U, .stride = 4U},
                                   .pixel_format = PixelFormat::mono8,
                                   .buffer = std::move(buffer)});
    EXPECT_TRUE(result);
    return std::move(result).value();
}

EventPersistenceRequest request(const std::string& event_id)
{
    const auto wall_base = WallClockTime{std::chrono::sys_days{std::chrono::year{2026} / 8 / 4}};
    auto first = frame("CAM01", 1U, 100ms);
    auto second = frame("CAM01", 2U, 200ms);
    KeyFrameDescriptor descriptor{
        .camera_id = "CAM01",
        .camera_frame_number = second.camera_frame_number(),
        .sequence_number = second.sequence_number(),
        .monotonic_time = second.received_monotonic_time(),
        .wall_clock_time = second.received_wall_clock_time(),
        .geometry = second.geometry(),
        .pixel_format = second.pixel_format(),
        .reasons = {KeyFrameReason::candidate_trigger, KeyFrameReason::highest_confidence}};
    EventPersistenceRequest value;
    value.metadata = {.event_id = event_id,
                      .event_state = "Confirmed",
                      .candidate_time = wall_base + 200ms,
                      .confirmed_time = wall_base + 250ms,
                      .start_time = wall_base + 100ms,
                      .end_time = wall_base + 300ms,
                      .camera_ids = {"CAM01"},
                      .trigger_camera_id = "CAM01",
                      .trigger_frame_number = 102U,
                      .trigger_reason = "ManualTest",
                      .confidence = 0.875,
                      .pre_event_duration = 100ms,
                      .post_event_duration = 100ms,
                      .algorithm_name = "mock-detector",
                      .algorithm_version = "m5",
                      .config_version = "42",
                      .machine_id = "PM-01",
                      .production_line_id = "LINE-A",
                      .paper_type = "test-paper",
                      .paper_speed = 900.5,
                      .upload_state = "Pending",
                      .time_quality = "Normal"};
    value.window = {.event_id = event_id,
                    .version = 3U,
                    .requested_start = MonotonicTime{100ms},
                    .requested_end = MonotonicTime{300ms},
                    .closed_monotonic_time = MonotonicTime{301ms},
                    .display_wall_clock_time = wall_base + 200ms,
                    .camera_windows = {{.camera_id = "CAM01",
                                        .requested_start = MonotonicTime{100ms},
                                        .requested_end = MonotonicTime{300ms},
                                        .available_start = MonotonicTime{100ms},
                                        .available_end = MonotonicTime{200ms},
                                        .first_sequence_number = 1U,
                                        .last_sequence_number = 2U,
                                        .frames = {first, second},
                                        .complete = true}},
                    .complete = true};
    value.key_frames.push_back({.descriptor = std::move(descriptor),
                                .jpeg = {std::byte{0xff}, std::byte{0xd8}, std::byte{0x01},
                                         std::byte{0xff}, std::byte{0xd9}}});
    return value;
}

EventPersistenceRequest performance_request(const std::string& event_id)
{
    constexpr std::uint32_t width = 1624U;
    constexpr std::uint32_t height = 1240U;
    constexpr std::size_t frame_count = 911U;
    constexpr auto frame_period = std::chrono::nanoseconds{16666667};
    auto buffer = std::make_shared<FrameBuffer>(static_cast<std::size_t>(width) * height);
    EXPECT_TRUE(buffer->set_size(buffer->capacity()));
    const auto wall_base = WallClockTime{std::chrono::sys_days{std::chrono::year{2026} / 8 / 9}};
    std::vector<FrameView> frames;
    frames.reserve(frame_count);
    for (std::size_t index = 0U; index < frame_count; ++index)
    {
        const auto offset = frame_period * static_cast<std::int64_t>(index);
        const auto monotonic_offset = std::chrono::duration_cast<MonotonicTime::duration>(offset);
        const auto wall_offset = std::chrono::duration_cast<WallClockTime::duration>(offset);
        auto made =
            make_frame_view({.camera_id = "CAM01",
                             .camera_frame_number = index + 1U,
                             .sequence_number = index + 1U,
                             .received_monotonic_time = MonotonicTime{monotonic_offset},
                             .received_wall_clock_time = wall_base + wall_offset,
                             .geometry = {.width = width, .height = height, .stride = width},
                             .pixel_format = PixelFormat::mono8,
                             .buffer = buffer});
        EXPECT_TRUE(made);
        frames.push_back(std::move(made).value());
    }
    const auto end = frames.back().received_monotonic_time();
    EventPersistenceRequest value;
    value.metadata = {.event_id = event_id,
                      .event_state = "Confirmed",
                      .candidate_time = wall_base + 8s,
                      .confirmed_time = wall_base + 9s,
                      .start_time = wall_base,
                      .end_time = wall_base + std::chrono::duration_cast<WallClockTime::duration>(
                                                  end.time_since_epoch()),
                      .camera_ids = {"CAM01"},
                      .trigger_camera_id = "CAM01",
                      .trigger_frame_number = 481U,
                      .trigger_reason = "PerformanceSimulation",
                      .confidence = 0.95,
                      .pre_event_duration = 8s,
                      .post_event_duration = 8s,
                      .algorithm_name = "mock-detector",
                      .algorithm_version = "performance",
                      .config_version = "42",
                      .machine_id = "PERF-EDGE",
                      .production_line_id = "PERF-LINE",
                      .paper_type = "simulation",
                      .paper_speed = 900.0,
                      .upload_state = "Pending",
                      .time_quality = "Normal"};
    value.window = {.event_id = event_id,
                    .version = 3U,
                    .requested_start = MonotonicTime{},
                    .requested_end = end,
                    .closed_monotonic_time = end + 1ms,
                    .display_wall_clock_time = wall_base + 8s,
                    .camera_windows = {{.camera_id = "CAM01",
                                        .requested_start = MonotonicTime{},
                                        .requested_end = end,
                                        .available_start = MonotonicTime{},
                                        .available_end = end,
                                        .first_sequence_number = 1U,
                                        .last_sequence_number = frame_count,
                                        .frames = std::move(frames),
                                        .complete = true}},
                    .complete = true};
    const auto& key = value.window.camera_windows.front().frames[480U];
    value.key_frames.push_back(
        {.descriptor = {.camera_id = key.camera_id(),
                        .camera_frame_number = key.camera_frame_number(),
                        .sequence_number = key.sequence_number(),
                        .monotonic_time = key.received_monotonic_time(),
                        .wall_clock_time = key.received_wall_clock_time(),
                        .geometry = key.geometry(),
                        .pixel_format = key.pixel_format(),
                        .reasons = {KeyFrameReason::candidate_trigger}},
         .jpeg = {std::byte{0xff}, std::byte{0xd8}, std::byte{0xff}, std::byte{0xd9}}});
    return value;
}

Error injected_io_error(const std::string& code)
{
    Error error = make_error("STORAGE_IO_FAILED", Severity::error, "injected I/O failure",
                             "storage-test", "inject", true);
    error.native_domain = "win32";
    error.native_code = code;
    return error;
}

class FaultingFileSystem final : public IEventFileSystem
{
  public:
    explicit FaultingFileSystem(std::shared_ptr<IEventFileSystem> delegate)
        : delegate_(std::move(delegate))
    {
    }

    Result<void> create_directories(const std::filesystem::path& path) override
    {
        return delegate_->create_directories(path);
    }

    Result<void> create_directory_exclusive(const std::filesystem::path& path) override
    {
        return delegate_->create_directory_exclusive(path);
    }

    Result<BufferedFileWriteResult> write_new_file_buffered(
        const std::filesystem::path& path, const std::span<const std::byte> contents,
        const std::stop_token stop_token) override
    {
        ++write_occurrences[path.filename().string()];
        if (!hash_failure_filename.empty() && path.filename() == hash_failure_filename)
        {
            auto error = injected_io_error("-1073741811");
            error.native_domain = "cng";
            return Result<BufferedFileWriteResult>::failure(std::move(error));
        }
        if (!fail_write_filename.empty() && path.filename() == fail_write_filename)
            return Result<BufferedFileWriteResult>::failure(injected_io_error(native_code));
        auto result = delegate_->write_new_file_buffered(path, contents, stop_token);
        if (result && !short_write_filename.empty() && path.filename() == short_write_filename &&
            result.value().bytes_written > 0U)
            --result.value().bytes_written;
        return result;
    }

    Result<BufferedFileWriteResult> write_new_raw_block_buffered(
        const std::filesystem::path& path, const std::span<const std::byte> contents,
        const std::stop_token stop_token) override
    {
        ++write_occurrences[path.filename().string()];
        if (!fail_write_filename.empty() && path.filename() == fail_write_filename)
            return Result<BufferedFileWriteResult>::failure(injected_io_error(native_code));
        auto result = delegate_->write_new_raw_block_buffered(path, contents, stop_token);
        if (result && stop_after_raw_block != nullptr)
            stop_after_raw_block->request_stop();
        return result;
    }

    Result<HashedFileContents> read_file_bounded_hashed(const std::filesystem::path& path,
                                                        const std::size_t maximum_bytes) override
    {
        const auto occurrence = ++read_occurrences[path.filename().string()];
        if (!fail_read_filename.empty() && path.filename() == fail_read_filename &&
            occurrence == fail_read_occurrence)
            return Result<HashedFileContents>::failure(injected_io_error(native_code));
        auto result = delegate_->read_file_bounded_hashed(path, maximum_bytes);
        if (result && !corrupt_read_filename.empty() && path.filename() == corrupt_read_filename &&
            !result.value().contents.empty())
        {
            result.value().contents.front() ^= std::byte{0xff};
            result.value().sha256.assign(64U, '0');
        }
        return result;
    }

    Result<std::uint64_t> file_size(const std::filesystem::path& path) override
    {
        return delegate_->file_size(path);
    }

    Result<std::vector<std::byte>> read_file_bounded(const std::filesystem::path& path,
                                                     const std::size_t maximum_bytes) override
    {
        const auto occurrence = ++read_occurrences[path.filename().string()];
        if (!fail_read_filename.empty() && path.filename() == fail_read_filename &&
            occurrence == fail_read_occurrence)
            return Result<std::vector<std::byte>>::failure(injected_io_error(native_code));
        auto result = delegate_->read_file_bounded(path, maximum_bytes);
        if (result && !corrupt_read_filename.empty() && path.filename() == corrupt_read_filename &&
            !result.value().empty())
            result.value().front() ^= std::byte{0xff};
        return result;
    }

    Result<EventPathKind> path_kind(const std::filesystem::path& path) override
    {
        return delegate_->path_kind(path);
    }

    Result<std::vector<std::filesystem::path>> list_directories_bounded(
        const std::filesystem::path& path, const std::size_t maximum_entries) override
    {
        return delegate_->list_directories_bounded(path, maximum_entries);
    }

    Result<void> move_directory_atomically(const std::filesystem::path& source,
                                           const std::filesystem::path& destination) override
    {
        if (fail_move)
            return Result<void>::failure(injected_io_error(native_code));
        return delegate_->move_directory_atomically(source, destination);
    }

    std::filesystem::path fail_write_filename;
    std::filesystem::path fail_read_filename;
    std::filesystem::path corrupt_read_filename;
    std::filesystem::path short_write_filename;
    std::filesystem::path hash_failure_filename;
    std::map<std::string, std::size_t> read_occurrences;
    std::map<std::string, std::size_t> write_occurrences;
    std::size_t fail_read_occurrence{1U};
    std::string native_code{"112"};
    bool fail_move{};
    std::stop_source* stop_after_raw_block{};

  private:
    std::shared_ptr<IEventFileSystem> delegate_;
};

std::unique_ptr<EventTransactionWriter> writer_for(
    const std::filesystem::path& root, const std::shared_ptr<IEventFileSystem>& file_system)
{
    auto result = EventTransactionWriter::create({.event_root = root}, file_system);
    EXPECT_TRUE(result);
    return std::move(result).value();
}

TEST(StorageEventStore, PersistsReplayableManifestWithChecksumsUnderChinesePath)
{
    TemporaryDirectory temporary{"persist"};
    auto writer = writer_for(temporary.path() / L"事件 数据", make_windows_event_file_system());

    auto persisted = writer->persist(request("019f-m506-persist-0001"));

    ASSERT_TRUE(persisted) << persisted.error().message;
    EXPECT_EQ(persisted.value().raw_block_count, 1U);
    EXPECT_EQ(persisted.value().raw_frame_count, 2U);
    EXPECT_EQ(persisted.value().raw_file_count, 1U);
    EXPECT_EQ(persisted.value().key_frame_count, 1U);
    EXPECT_TRUE(std::filesystem::is_directory(persisted.value().committed_directory));
    auto verified = writer->verify_committed_manifest(persisted.value().committed_directory);
    ASSERT_TRUE(verified) << verified.error().message;
    const auto manifest = nlohmann::json::parse(verified.value());
    EXPECT_EQ(manifest["schemaVersion"], 3U);
    EXPECT_EQ(manifest["writeMode"], "buffered");
    EXPECT_EQ(manifest["powerLossDurable"], false);
    EXPECT_EQ(manifest["verification"], "upload-or-on-demand");
    EXPECT_EQ(manifest["eventId"], "019f-m506-persist-0001");
    EXPECT_EQ(manifest["decisionState"], "Confirmed");
    EXPECT_EQ(manifest["triggerCount"], 1U);
    EXPECT_EQ(manifest["candidateTime"], "2026-08-04T00:00:00.200Z");
    EXPECT_EQ(manifest["cameraIds"].size(), 1U);
    ASSERT_EQ(manifest["rawBlocks"].size(), 1U);
    EXPECT_EQ(manifest["rawBlocks"][0]["frameCount"], 2U);
    EXPECT_EQ(manifest["rawBlocks"][0]["firstSequenceNumber"], 1U);
    EXPECT_EQ(manifest["rawBlocks"][0]["lastSequenceNumber"], 2U);
    EXPECT_EQ(manifest["keyFrames"].size(), 1U);
    EXPECT_EQ(manifest["keyFrames"][0]["reasons"].size(), 2U);
    EXPECT_EQ(manifest["fileChecksums"].size(), 3U);
    EXPECT_EQ(manifest["fileSizes"].size(), 3U);
    for (const auto& [name, checksum] : manifest["fileChecksums"].items())
    {
        EXPECT_TRUE(checksum.get<std::string>().starts_with("sha256:"));
        EXPECT_TRUE(std::filesystem::is_regular_file(persisted.value().committed_directory / name));
    }
    const auto block_path =
        persisted.value().committed_directory / manifest["rawBlocks"][0]["path"].get<std::string>();
    EXPECT_EQ(manifest["fileChecksums"][manifest["rawBlocks"][0]["path"].get<std::string>()],
              manifest["rawBlocks"][0]["sha256"]);
    EXPECT_EQ(std::filesystem::file_size(block_path),
              manifest["rawBlocks"][0]["sizeBytes"].get<std::uint64_t>());
    std::ifstream block{block_path, std::ios::binary};
    std::array<char, 8U> magic{};
    block.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    EXPECT_EQ(std::string(magic.data(), 7U), "PBNVME2");
    EXPECT_FALSE(std::filesystem::exists(persisted.value().transaction_directory));
}

TEST(StorageEventStore, WindowsBufferedWriterUsesCngSha256VectorsAndRejectsOverwrite)
{
    TemporaryDirectory temporary{"cng-sha"};
    auto file_system = make_windows_event_file_system();
    const std::vector<std::byte> empty;
    const std::string abc_text{"abc"};
    const auto abc = std::as_bytes(std::span{abc_text});
    std::vector<std::byte> multi(2U * 1024U * 1024U, std::byte{0x5a});

    auto empty_written =
        file_system->write_new_file_buffered(temporary.path() / "empty.bin", empty, {});
    ASSERT_TRUE(empty_written);
    EXPECT_EQ(empty_written.value().bytes_written, 0U);
    EXPECT_EQ(empty_written.value().sha256,
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    auto abc_written = file_system->write_new_file_buffered(temporary.path() / "abc.bin", abc, {});
    ASSERT_TRUE(abc_written);
    EXPECT_EQ(abc_written.value().sha256,
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    auto multi_written =
        file_system->write_new_file_buffered(temporary.path() / "multi.bin", multi, {});
    ASSERT_TRUE(multi_written);
    auto read = file_system->read_file_bounded_hashed(temporary.path() / "multi.bin", multi.size());
    ASSERT_TRUE(read);
    EXPECT_EQ(multi_written.value().sha256, read.value().sha256);
    EXPECT_EQ(multi_written.value().bytes_written, multi.size());

    auto overwrite = file_system->write_new_file_buffered(temporary.path() / "abc.bin", abc, {});
    ASSERT_FALSE(overwrite);
    std::stop_source cancelled;
    cancelled.request_stop();
    auto cancelled_write = file_system->write_new_file_buffered(temporary.path() / "cancelled.bin",
                                                                abc, cancelled.get_token());
    ASSERT_FALSE(cancelled_write);
    EXPECT_EQ(cancelled_write.error().business_code, "STORAGE_IO_FAILED");
}

TEST(StorageEventStore, PersistenceWritesAndHashesEachFileOnceWithoutPayloadReadback)
{
    TemporaryDirectory temporary{"single-pass"};
    auto file_system = std::make_shared<FaultingFileSystem>(make_windows_event_file_system());
    auto writer = writer_for(temporary.path(), file_system);

    auto persisted = writer->persist(request("019f-m506-single-pass"));

    ASSERT_TRUE(persisted);
    EXPECT_TRUE(file_system->read_occurrences.empty());
    EXPECT_EQ(file_system->write_occurrences["block-0.pbnvme"], 1U);
    EXPECT_EQ(file_system->write_occurrences["event.json"], 1U);
    EXPECT_EQ(file_system->write_occurrences["keyframe-0.jpg"], 1U);
    EXPECT_EQ(file_system->write_occurrences["manifest.json"], 1U);
}

TEST(StorageEventStore, ShortWriteAndHashApiFailureKeepTransactionIncomplete)
{
    for (const auto hash_failure : {false, true})
    {
        TemporaryDirectory temporary{hash_failure ? "hash-api-failure" : "short-write"};
        auto file_system = std::make_shared<FaultingFileSystem>(make_windows_event_file_system());
        if (hash_failure)
            file_system->hash_failure_filename = "event.json";
        else
            file_system->short_write_filename = "event.json";
        auto writer = writer_for(temporary.path(), file_system);
        const auto event_id = hash_failure ? "019f-m506-hash-failure" : "019f-m506-short-write";

        auto persisted = writer->persist(request(event_id));

        ASSERT_FALSE(persisted);
        EXPECT_EQ(persisted.error().business_code,
                  hash_failure ? "EVENT_WRITE_FAILED" : "EVENT_CHECKSUM_FAILED");
        EXPECT_FALSE(std::filesystem::exists(temporary.path() / "2026" / "08" / "04" / event_id));
        EXPECT_TRUE(std::filesystem::exists(temporary.path() / ".transactions"));
    }
}

TEST(StorageEventInspector, ReadsManifestOrderTracesKeyFramesAndExportsVerifiedZip)
{
    TemporaryDirectory temporary{"inspect-export"};
    const auto root = temporary.path() / L"事件 导出";
    auto file_system = std::make_shared<FaultingFileSystem>(make_windows_event_file_system());
    auto writer = writer_for(root, file_system);
    auto persisted = writer->persist(request("019f-m509-inspect-0001"));
    ASSERT_TRUE(persisted);
    auto inspector = EventInspector::create({.event_root = root}, file_system);
    ASSERT_TRUE(inspector);
    const auto relative = persisted.value().committed_directory.lexically_relative(root);

    const auto manifest = nlohmann::json::parse(persisted.value().manifest_json);
    const auto raw_filename =
        std::filesystem::path{manifest["rawBlocks"][0]["path"].get<std::string>()}
            .filename()
            .string();
    const auto key_filename =
        std::filesystem::path{manifest["keyFrames"][0]["path"].get<std::string>()}
            .filename()
            .string();
    auto summary = inspector.value()->inspect_summary(relative);

    ASSERT_TRUE(summary) << summary.error().message;
    EXPECT_EQ(summary.value().event_id, "019f-m509-inspect-0001");
    EXPECT_EQ(summary.value().raw_frame_count, 2U);
    EXPECT_EQ(summary.value().key_frame_count, 1U);
    EXPECT_EQ(summary.value().observed_sequence_gaps, 0U);
    EXPECT_TRUE(summary.value().key_frames_traceable);
    EXPECT_FALSE(summary.value().thumbnail_jpeg.empty());
    EXPECT_EQ(file_system->read_occurrences[raw_filename], 0U);
    EXPECT_EQ(file_system->read_occurrences["event.json"], 0U);
    EXPECT_EQ(file_system->read_occurrences[key_filename], 1U);

    auto inspected = inspector.value()->inspect(relative);

    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_EQ(inspected.value().event_id, "019f-m509-inspect-0001");
    ASSERT_EQ(inspected.value().raw_frames.size(), 2U);
    EXPECT_EQ(inspected.value().raw_frames[0].sequence_number, 1U);
    EXPECT_EQ(inspected.value().raw_frames[1].sequence_number, 2U);
    EXPECT_EQ(inspected.value().observed_sequence_gaps, 0U);
    EXPECT_TRUE(inspected.value().key_frames_traceable);
    ASSERT_EQ(inspected.value().key_frames.size(), 1U);
    EXPECT_EQ(inspected.value().key_frames[0].sequence_number, 2U);
    EXPECT_FALSE(inspected.value().thumbnail_jpeg.empty());

    auto archive = inspector.value()->export_zip(relative);
    ASSERT_TRUE(archive) << archive.error().message;
    EXPECT_EQ(archive.value().source_file_count, 4U);
    ASSERT_GE(archive.value().zip.size(), 4U);
    EXPECT_EQ(archive.value().zip[0], std::byte{0x50});
    EXPECT_EQ(archive.value().zip[1], std::byte{0x4b});

    const auto destination = temporary.path() / L"导出 目标" / L"已校验事件.zip";
    auto file_archive = inspector.value()->export_zip_file(relative, destination);
    ASSERT_TRUE(file_archive) << file_archive.error().message;
    EXPECT_EQ(file_archive.value().source_file_count, 4U);
    EXPECT_EQ(file_archive.value().path, destination);
    std::ifstream input{destination, std::ios::binary};
    std::array<unsigned char, 2U> signature{};
    input.read(reinterpret_cast<char*>(signature.data()),
               static_cast<std::streamsize>(signature.size()));
    EXPECT_EQ(signature[0], 0x50U);
    EXPECT_EQ(signature[1], 0x4bU);
    input.clear();
    input.seekg(0);
    const std::vector<unsigned char> streamed{std::istreambuf_iterator<char>{input},
                                              std::istreambuf_iterator<char>{}};
    ASSERT_EQ(streamed.size(), archive.value().zip.size());
    for (std::size_t index = 0U; index < streamed.size(); ++index)
        EXPECT_EQ(streamed[index], std::to_integer<unsigned char>(archive.value().zip[index]));
}

TEST(StorageEventInspector, KeepsManifestV2AndPbnvme1ReadOnlyInspectionAndExportCompatibility)
{
    TemporaryDirectory temporary{"legacy-v2"};
    const auto root = temporary.path() / L"历史 事件";
    auto file_system = make_windows_event_file_system();
    auto writer = writer_for(root, file_system);
    auto persisted = writer->persist(request("019f-m509-legacy-v2"));
    ASSERT_TRUE(persisted);
    const auto manifest_path = persisted.value().committed_directory / "manifest.json";
    nlohmann::json manifest;
    {
        std::ifstream input{manifest_path};
        input >> manifest;
    }
    const auto raw_relative = manifest["rawBlocks"][0]["path"].get<std::string>();
    const auto raw_path = persisted.value().committed_directory / raw_relative;
    std::vector<std::byte> bytes(static_cast<std::size_t>(std::filesystem::file_size(raw_path)));
    {
        std::ifstream input{raw_path, std::ios::binary};
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        ASSERT_TRUE(input);
    }
    auto all = std::span<std::byte>{bytes};
    all[6U] = std::byte{'1'};
    all[8U] = std::byte{1U};
    all[9U] = std::byte{0U};
    test_put_u32(all, 128U, 0U);
    const auto header_crc = crc32c(all.first(nvme_page_bytes));
    test_put_u32(all, 128U, header_crc);
    const auto frame_count = manifest["rawBlocks"][0]["frameCount"].get<std::size_t>();
    std::uint32_t data_crc{};
    for (std::size_t ordinal = 0U; ordinal < frame_count; ++ordinal)
    {
        auto entry =
            all.subspan(nvme_page_bytes + ordinal * nvme_index_entry_bytes, nvme_index_entry_bytes);
        const auto data_offset = test_little_u64(entry, 48U);
        const auto data_size = test_little_u32(entry, 56U);
        const auto frame_bytes = std::span<const std::byte>{bytes}.subspan(
            static_cast<std::size_t>(data_offset), data_size);
        test_put_u32(entry, 76U, crc32c(frame_bytes));
        test_put_u32(entry, 80U, 0U);
        test_put_u32(entry, 80U, crc32c(entry));
        data_crc = crc32c(frame_bytes, data_crc);
    }
    const auto index_crc = crc32c(std::span<const std::byte>{bytes}.subspan(
        nvme_page_bytes, frame_count * nvme_index_entry_bytes));
    const auto footer_offset = bytes.size() - nvme_page_bytes;
    all[footer_offset + 8U] = std::byte{1U};
    all[footer_offset + 9U] = std::byte{0U};
    test_put_u32(all, footer_offset + 56U, index_crc);
    test_put_u32(all, footer_offset + 60U, data_crc);
    test_put_u32(all, footer_offset + 64U, header_crc);
    all[bytes.size() - 2U] = std::byte{'1'};
    test_put_u32(all, footer_offset + 4084U, 0U);
    const auto footer_crc = crc32c(std::span<const std::byte>{bytes}.last(nvme_page_bytes));
    test_put_u32(all, footer_offset + 4084U, footer_crc);
    {
        std::ofstream output{raw_path, std::ios::binary | std::ios::trunc};
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        ASSERT_TRUE(output);
    }
    auto legacy_hash = file_system->read_file_bounded_hashed(raw_path, bytes.size());
    ASSERT_TRUE(legacy_hash);
    manifest["schemaVersion"] = 2U;
    manifest.erase("writeMode");
    manifest.erase("powerLossDurable");
    manifest.erase("verification");
    manifest["rawBlocks"][0]["headerCrc32c"] = header_crc;
    manifest["rawBlocks"][0]["indexCrc32c"] = index_crc;
    manifest["rawBlocks"][0]["dataCrc32c"] = data_crc;
    manifest["rawBlocks"][0]["footerCrc32c"] = footer_crc;
    manifest["rawBlocks"][0]["sha256"] = "sha256:" + legacy_hash.value().sha256;
    manifest["fileChecksums"][raw_relative] = "sha256:" + legacy_hash.value().sha256;
    {
        std::ofstream output{manifest_path, std::ios::trunc};
        output << manifest.dump(2) << '\n';
        ASSERT_TRUE(output);
    }

    auto inspector = EventInspector::create({.event_root = root}, file_system);
    ASSERT_TRUE(inspector);
    const auto relative = persisted.value().committed_directory.lexically_relative(root);
    auto inspected = inspector.value()->inspect(relative);
    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_EQ(inspected.value().raw_frames.size(), 2U);
    auto archive = inspector.value()->export_zip(relative);
    ASSERT_TRUE(archive) << archive.error().message;
    EXPECT_FALSE(archive.value().zip.empty());
}

TEST(StorageEventInspector, RejectsPendingDamagedOversizedAndInterruptedExports)
{
    TemporaryDirectory temporary{"inspect-failures"};
    const auto root = temporary.path() / L"事件 导出";
    auto file_system = std::make_shared<FaultingFileSystem>(make_windows_event_file_system());
    auto writer = writer_for(root, file_system);
    auto persisted = writer->persist(request("019f-m509-inspect-0002"));
    ASSERT_TRUE(persisted);
    const auto relative = persisted.value().committed_directory.lexically_relative(root);

    auto tiny =
        EventInspector::create({.event_root = root, .maximum_export_bytes = 1024U}, file_system);
    ASSERT_TRUE(tiny);
    auto oversized = tiny.value()->export_zip(relative);
    ASSERT_FALSE(oversized);
    EXPECT_EQ(oversized.error().business_code, "EVENT_EXPORT_TOO_LARGE");

    file_system->read_occurrences.clear();
    file_system->fail_read_filename = "block-0.pbnvme";
    file_system->fail_read_occurrence = 1U;
    auto inspector = EventInspector::create({.event_root = root}, file_system);
    ASSERT_TRUE(inspector);
    auto interrupted = inspector.value()->export_zip(relative);
    ASSERT_FALSE(interrupted);
    EXPECT_EQ(interrupted.error().business_code, "STORAGE_IO_FAILED");
    file_system->fail_read_filename.clear();

    file_system->read_occurrences.clear();
    file_system->fail_read_filename = "block-0.pbnvme";
    file_system->fail_read_occurrence = 1U;
    const auto interrupted_path = temporary.path() / L"中文 目标" / L"中断.zip";
    auto interrupted_file = inspector.value()->export_zip_file(relative, interrupted_path);
    ASSERT_FALSE(interrupted_file);
    EXPECT_EQ(interrupted_file.error().business_code, "STORAGE_IO_FAILED");
    EXPECT_FALSE(std::filesystem::exists(interrupted_path));
    EXPECT_FALSE(std::filesystem::exists(interrupted_path.wstring() + L".partial"));
    file_system->fail_read_filename.clear();

    auto hidden = inspector.value()->inspect(std::filesystem::path{".transactions"} /
                                             "019f-m509-hidden.pending");
    ASSERT_FALSE(hidden);
    EXPECT_EQ(hidden.error().business_code, "EVENT_NOT_FOUND");

    const auto raw_path =
        persisted.value().committed_directory / "raw" / "camera-0" / "block-0.pbnvme";
    {
        std::ofstream output{raw_path, std::ios::binary | std::ios::app};
        output << "damage";
    }
    auto damaged = inspector.value()->inspect(relative);
    ASSERT_FALSE(damaged);
    EXPECT_TRUE(damaged.error().business_code == "EVENT_CHECKSUM_FAILED" ||
                damaged.error().business_code == "EVENT_RECOVERY_FAILED");
}

TEST(StorageEventStore, GroupsCurrentTwentySecondScenarioIntoOneSecondBlocks)
{
    TemporaryDirectory temporary{"twenty-second-blocks"};
    auto writer = writer_for(temporary.path(), make_windows_event_file_system());
    auto value = request("019f-m7-blocks-0907");
    auto& camera_window = value.window.camera_windows.front();
    camera_window.frames.clear();
    camera_window.frames.reserve(907U);
    for (std::uint64_t index = 0U; index < 907U; ++index)
        camera_window.frames.push_back(
            frame("CAM01", index + 1U, std::chrono::milliseconds{index * 22U}));
    camera_window.requested_start = MonotonicTime{0ms};
    camera_window.requested_end = MonotonicTime{20s};
    camera_window.available_start = MonotonicTime{0ms};
    camera_window.available_end = MonotonicTime{19932ms};
    camera_window.first_sequence_number = 1U;
    camera_window.last_sequence_number = 907U;
    value.key_frames.front().descriptor.monotonic_time =
        camera_window.frames[1].received_monotonic_time();
    value.key_frames.front().descriptor.wall_clock_time =
        camera_window.frames[1].received_wall_clock_time();
    value.window.requested_start = MonotonicTime{0ms};
    value.window.requested_end = MonotonicTime{20s};
    value.window.closed_monotonic_time = MonotonicTime{20001ms};

    const auto started = std::chrono::steady_clock::now();
    auto persisted = writer->persist(value);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    ASSERT_TRUE(persisted) << persisted.error().message;
    EXPECT_EQ(persisted.value().raw_frame_count, 907U);
    EXPECT_EQ(persisted.value().raw_block_count, 20U);
    EXPECT_LT(persisted.value().raw_block_count, persisted.value().raw_frame_count);
    auto verified = writer->verify_committed_manifest(persisted.value().committed_directory);
    ASSERT_TRUE(verified);
    const auto manifest = nlohmann::json::parse(verified.value());
    ASSERT_EQ(manifest["rawBlocks"].size(), 20U);
    std::uint64_t frame_count = 0U;
    for (const auto& block : manifest["rawBlocks"])
    {
        EXPECT_LE(block["frameCount"].get<std::uint64_t>(), event_raw_block_maximum_frames);
        frame_count += block["frameCount"].get<std::uint64_t>();
    }
    EXPECT_EQ(frame_count, 907U);
    const auto elapsed_seconds = std::chrono::duration<double>{elapsed}.count();
    RecordProperty("elapsedMilliseconds",
                   std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
    RecordProperty("writtenBytes", persisted.value().bytes_written);
    RecordProperty("measuredMiBPerSecond",
                   elapsed_seconds > 0.0 ? static_cast<double>(persisted.value().bytes_written) /
                                               (1024.0 * 1024.0 * elapsed_seconds)
                                         : 0.0);
}

TEST(StorageEventStore, RejectsUnsafeIdentityBudgetOverflowAndUntraceableKeyFrameBeforeIo)
{
    TemporaryDirectory temporary{"validation"};
    auto writer = writer_for(temporary.path(), make_windows_event_file_system());

    auto unsafe = request("../unsafe-event");
    auto unsafe_result = writer->persist(unsafe);
    ASSERT_FALSE(unsafe_result);
    EXPECT_EQ(unsafe_result.error().business_code, "EVENT_WRITE_FAILED");

    auto untraceable = request("019f-m506-untraceable");
    untraceable.key_frames[0].descriptor.sequence_number = 999U;
    auto untraceable_result = writer->persist(untraceable);
    ASSERT_FALSE(untraceable_result);
    EXPECT_EQ(untraceable_result.error().business_code, "EVENT_WRITE_FAILED");

    auto small_writer_result = EventTransactionWriter::create(
        {.event_root = temporary.path() / "small", .maximum_raw_frames = 1U},
        make_windows_event_file_system());
    ASSERT_TRUE(small_writer_result);
    auto overflow = std::move(small_writer_result).value()->persist(request("019f-m506-overflow"));
    ASSERT_FALSE(overflow);
    EXPECT_EQ(overflow.error().business_code, "EVENT_WRITE_FAILED");
    EXPECT_FALSE(std::filesystem::exists(temporary.path() / ".transactions"));
}

TEST(StorageEventStore, CancellationKeepsRecoverableTransactionAndPublishesNoEventDirectory)
{
    TemporaryDirectory temporary{"cancel-block-write"};
    auto file_system = std::make_shared<FaultingFileSystem>(make_windows_event_file_system());
    std::stop_source stop;
    file_system->stop_after_raw_block = &stop;
    auto writer = writer_for(temporary.path(), file_system);
    auto value = request("019f-m7-cancel-0001");
    auto& frames = value.window.camera_windows.front().frames;
    frames.push_back(frame("CAM01", 3U, 1200ms));
    value.window.camera_windows.front().available_end = MonotonicTime{1200ms};
    value.window.camera_windows.front().requested_end = MonotonicTime{1300ms};
    value.window.camera_windows.front().last_sequence_number = 3U;
    value.window.requested_end = MonotonicTime{1300ms};
    value.window.closed_monotonic_time = MonotonicTime{1301ms};

    auto persisted = writer->persist(value, stop.get_token());

    ASSERT_FALSE(persisted);
    EXPECT_EQ(persisted.error().business_code, "EVENT_WRITE_CANCELLED");
    EXPECT_FALSE(
        std::filesystem::exists(temporary.path() / "2026" / "08" / "04" / value.metadata.event_id));
    const auto pending_root = temporary.path() / ".transactions";
    ASSERT_TRUE(std::filesystem::is_directory(pending_root));
    std::size_t pending_count = 0U;
    for (const auto& entry : std::filesystem::directory_iterator{pending_root})
        pending_count += entry.is_directory() ? 1U : 0U;
    EXPECT_EQ(pending_count, 1U);
}

TEST(StorageEventStore, RetainsTransactionAndNeverPublishesOnEveryCommitWritePointFailure)
{
    const std::vector<std::filesystem::path> write_points{"event.json", "block-0.pbnvme",
                                                          "keyframe-0.jpg", "manifest.json"};
    for (std::size_t index = 0U; index < write_points.size() + 1U; ++index)
    {
        TemporaryDirectory temporary{"write-point-" + std::to_string(index)};
        auto file_system = std::make_shared<FaultingFileSystem>(make_windows_event_file_system());
        if (index < write_points.size())
            file_system->fail_write_filename = write_points[index];
        else
            file_system->fail_move = true;
        auto writer = writer_for(temporary.path(), file_system);
        const auto event_id = "019f-m506-failure-000" + std::to_string(index);

        auto persisted = writer->persist(request(event_id));

        ASSERT_FALSE(persisted) << index;
        EXPECT_EQ(persisted.error().business_code, "EVENT_WRITE_FAILED") << index;
        EXPECT_EQ(persisted.error().severity, Severity::critical) << index;
        EXPECT_FALSE(std::filesystem::exists(temporary.path() / "2026" / "08" / "04" / event_id));
        std::size_t pending_count = 0U;
        for (const auto& entry :
             std::filesystem::directory_iterator{temporary.path() / ".transactions"})
            pending_count += entry.is_directory() ? 1U : 0U;
        EXPECT_EQ(pending_count, 1U) << index;
    }
}

TEST(StorageEventStore, MapsDiskFullAndAccessDeniedWithoutLosingNativeDiagnostics)
{
    for (const auto& native_code : {std::string{"112"}, std::string{"5"}})
    {
        TemporaryDirectory temporary{"native-" + native_code};
        auto file_system = std::make_shared<FaultingFileSystem>(make_windows_event_file_system());
        file_system->fail_write_filename = "event.json";
        file_system->native_code = native_code;
        auto writer = writer_for(temporary.path(), file_system);

        auto persisted = writer->persist(request("019f-m506-native-" + native_code));

        ASSERT_FALSE(persisted);
        EXPECT_EQ(persisted.error().business_code, "EVENT_WRITE_FAILED");
        ASSERT_TRUE(persisted.error().native_code.has_value());
        EXPECT_EQ(*persisted.error().native_code, native_code);
    }
}

TEST(StorageEventStore, DetectsCommittedBlockChecksumMismatch)
{
    TemporaryDirectory temporary{"checksum"};
    auto file_system = std::make_shared<FaultingFileSystem>(make_windows_event_file_system());
    file_system->corrupt_read_filename = "block-0.pbnvme";
    auto writer = writer_for(temporary.path(), file_system);

    auto persisted = writer->persist(request("019f-m506-checksum-0001"));

    ASSERT_TRUE(persisted);
    auto inspector = EventInspector::create({.event_root = temporary.path()}, file_system);
    ASSERT_TRUE(inspector);
    auto damaged = inspector.value()->inspect(
        persisted.value().committed_directory.lexically_relative(temporary.path()));
    ASSERT_FALSE(damaged);
    EXPECT_EQ(damaged.error().business_code, "EVENT_INTEGRITY_FAILED");
}

TEST(StorageEventStore, StartupRecoveryCommitsOnlyCompleteValidatedResidualTransaction)
{
    TemporaryDirectory temporary{"recover-complete"};
    auto file_system = std::make_shared<FaultingFileSystem>(make_windows_event_file_system());
    file_system->fail_move = true;
    auto first_writer = writer_for(temporary.path(), file_system);
    auto interrupted = first_writer->persist(request("019f-m506-recover-0001"));
    ASSERT_FALSE(interrupted);
    file_system->fail_move = false;
    auto recovery_writer = writer_for(temporary.path(), file_system);

    auto recovery = recovery_writer->recover_pending();

    ASSERT_TRUE(recovery) << recovery.error().message;
    EXPECT_EQ(recovery.value().scanned, 1U);
    EXPECT_EQ(recovery.value().committed, 1U);
    EXPECT_EQ(recovery.value().quarantined, 0U);
    ASSERT_EQ(recovery.value().items.size(), 1U);
    EXPECT_EQ(recovery.value().items[0].disposition, EventRecoveryDisposition::committed);
    EXPECT_TRUE(
        recovery_writer->verify_committed_manifest(recovery.value().items[0].resulting_directory));
}

TEST(StorageEventStore, StartupRecoveryQuarantinesMissingAndUnsupportedManifests)
{
    TemporaryDirectory temporary{"recover-damaged"};
    auto file_system = std::make_shared<FaultingFileSystem>(make_windows_event_file_system());
    file_system->fail_write_filename = "manifest.json";
    auto first_writer = writer_for(temporary.path(), file_system);
    ASSERT_FALSE(first_writer->persist(request("019f-m506-missing-0001")));
    file_system->fail_write_filename.clear();

    auto recovery_writer = writer_for(temporary.path(), file_system);
    auto missing = recovery_writer->recover_pending();
    ASSERT_TRUE(missing);
    ASSERT_EQ(missing.value().quarantined, 1U);
    EXPECT_EQ(missing.value().items[0].reason, "manifest-missing");
    EXPECT_TRUE(std::filesystem::is_regular_file(missing.value().items[0].resulting_directory /
                                                 "recovery.json"));

    file_system->fail_move = true;
    ASSERT_FALSE(first_writer->persist(request("019f-m506-schema-0001")));
    file_system->fail_move = false;
    auto transaction_iterator =
        std::filesystem::directory_iterator{temporary.path() / ".transactions"};
    ASSERT_NE(transaction_iterator, std::filesystem::directory_iterator{});
    const auto manifest_path = transaction_iterator->path() / "manifest.json";
    nlohmann::json manifest;
    {
        std::ifstream input{manifest_path};
        input >> manifest;
    }
    manifest["schemaVersion"] = 99U;
    {
        std::ofstream output{manifest_path, std::ios::trunc};
        output << manifest.dump(2) << '\n';
    }

    auto unsupported = recovery_writer->recover_pending();

    ASSERT_TRUE(unsupported);
    ASSERT_EQ(unsupported.value().quarantined, 1U);
    EXPECT_EQ(unsupported.value().items[0].reason, "EVENT_SCHEMA_UNSUPPORTED");
}

TEST(StorageEventStore, StartupRecoveryPublishesLengthCorrectTransactionWithoutReadingPayload)
{
    TemporaryDirectory temporary{"recover-checksum"};
    auto file_system = std::make_shared<FaultingFileSystem>(make_windows_event_file_system());
    file_system->fail_move = true;
    auto first_writer = writer_for(temporary.path(), file_system);
    const auto event_id = "019f-m506-recovery-checksum";
    ASSERT_FALSE(first_writer->persist(request(event_id)));
    file_system->fail_move = false;
    auto transaction_iterator =
        std::filesystem::directory_iterator{temporary.path() / ".transactions"};
    ASSERT_NE(transaction_iterator, std::filesystem::directory_iterator{});
    const auto raw_path = transaction_iterator->path() / "raw" / "camera-0" / "block-0.pbnvme";
    {
        std::fstream raw{raw_path, std::ios::in | std::ios::out | std::ios::binary};
        ASSERT_TRUE(raw);
        raw.put(static_cast<char>(0xff));
    }
    auto recovery_writer = writer_for(temporary.path(), file_system);

    auto recovery = recovery_writer->recover_pending();

    ASSERT_TRUE(recovery);
    ASSERT_EQ(recovery.value().committed, 1U);
    EXPECT_EQ(recovery.value().items[0].disposition, EventRecoveryDisposition::committed);
    EXPECT_TRUE(std::filesystem::exists(temporary.path() / "2026" / "08" / "04" / event_id));
}

TEST(StorageEventStorePerformance, ReleaseCommits911Mono8FramesAbove100MiBpsWithoutIndexReadback)
{
#ifndef NDEBUG
    GTEST_SKIP() << "Release-only 1.709 GiB persistence acceptance";
#else
    TemporaryDirectory temporary{"release-performance"};
    const auto event_root = temporary.path() / "events";
    auto file_system = std::make_shared<FaultingFileSystem>(make_windows_event_file_system());
    auto writer = EventTransactionWriter::create({.event_root = event_root}, file_system);
    ASSERT_TRUE(writer);
    std::mutex completion_mutex;
    std::condition_variable completion_condition;
    std::optional<EventPersistenceCompletion> completion;
    auto runtime = EventPersistenceRuntime::create(std::move(writer).value(),
                                                   [&](EventPersistenceCompletion value) {
                                                       {
                                                           std::scoped_lock lock{completion_mutex};
                                                           completion = std::move(value);
                                                       }
                                                       completion_condition.notify_all();
                                                   });
    ASSERT_TRUE(runtime);
    ASSERT_TRUE(runtime.value()->start());
    auto value = performance_request("019f-m506-release-performance");
    FILETIME creation{}, exit{}, kernel_before{}, user_before{};
    ASSERT_TRUE(
        GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel_before, &user_before));
    const auto started = std::chrono::steady_clock::now();
    ASSERT_TRUE(runtime.value()->submit(std::move(value)));
    {
        std::unique_lock lock{completion_mutex};
        ASSERT_TRUE(
            completion_condition.wait_for(lock, 30s, [&] { return completion.has_value(); }));
    }
    const auto finished = std::chrono::steady_clock::now();
    FILETIME kernel_after{}, user_after{};
    ASSERT_TRUE(GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel_after, &user_after));
    const auto snapshot = runtime.value()->snapshot();
    runtime.value()->request_stop();
    ASSERT_TRUE(runtime.value()->join(std::chrono::steady_clock::now() + 5s));
    ASSERT_TRUE(completion->outcome.has_value())
        << (completion->error ? completion->error->business_code : "missing outcome");

    auto database = EventMetadataDatabase::open({.database_path = temporary.path() / "events.db",
                                                 .event_root = event_root,
                                                 .backup_directory = temporary.path() / "backups"});
    ASSERT_TRUE(database);
    ASSERT_TRUE(database.value()->index_committed_manifest(completion->outcome->committed_directory,
                                                           completion->outcome->manifest_json));
    EXPECT_TRUE(file_system->read_occurrences.empty());

    const auto filetime_ticks = [](const FILETIME value) {
        ULARGE_INTEGER converted{};
        converted.LowPart = value.dwLowDateTime;
        converted.HighPart = value.dwHighDateTime;
        return converted.QuadPart;
    };
    const auto elapsed_seconds = std::chrono::duration<double>(finished - started).count();
    const auto cpu_seconds =
        static_cast<double>(filetime_ticks(kernel_after) - filetime_ticks(kernel_before) +
                            filetime_ticks(user_after) - filetime_ticks(user_before)) /
        10000000.0;
    const auto cpu_percent = elapsed_seconds > 0.0 ? cpu_seconds / elapsed_seconds * 100.0 : 0.0;
    std::cout << "PERF eventBytes=" << snapshot.last_write_bytes
              << " elapsedMs=" << snapshot.last_write_duration.count()
              << " throughputMiBps=" << snapshot.last_write_mib_per_second
              << " processCpuPercent=" << cpu_percent
              << " queueHighWatermark=" << snapshot.high_watermark
              << " persistenceRejected=" << snapshot.rejected
              << " generatedFrames=911 generatedFrameDrops=0 indexPayloadReads="
              << file_system->read_occurrences.size() << '\n';
    EXPECT_GE(snapshot.last_write_bytes, 1624ULL * 1240ULL * 911ULL);
    EXPECT_GE(snapshot.last_write_mib_per_second, 100.0);
    EXPECT_LE(snapshot.last_write_duration, 18s);
    EXPECT_EQ(snapshot.high_watermark, 1U);
    EXPECT_EQ(snapshot.rejected, 0U);
#endif
}

class BlockingWriter final : public IEventTransactionWriter
{
  public:
    Result<EventPersistenceOutcome> persist(const EventPersistenceRequest& value) override
    {
        std::unique_lock lock{mutex_};
        ++entered_;
        entered_condition_.notify_all();
        release_condition_.wait(lock, [this] { return released_; });
        return Result<EventPersistenceOutcome>::success(
            {.event_id = value.metadata.event_id,
             .committed_directory = value.metadata.event_id,
             .manifest_json = "{}"});
    }

    void wait_until_entered(const std::size_t count)
    {
        std::unique_lock lock{mutex_};
        ASSERT_TRUE(
            entered_condition_.wait_for(lock, 2s, [this, count] { return entered_ >= count; }));
    }

    void release()
    {
        {
            std::scoped_lock lock{mutex_};
            released_ = true;
        }
        release_condition_.notify_all();
    }

  private:
    std::mutex mutex_;
    std::condition_variable entered_condition_;
    std::condition_variable release_condition_;
    std::size_t entered_{};
    bool released_{};
};

TEST(StorageEventStore, BoundedRuntimeRejectsExactlyWhenFullAndDrainsAcceptedJobs)
{
    auto writer = std::make_unique<BlockingWriter>();
    auto* writer_pointer = writer.get();
    std::mutex callback_mutex;
    std::condition_variable callback_condition;
    std::size_t callbacks = 0U;
    auto runtime_result =
        EventPersistenceRuntime::create(std::move(writer),
                                        [&](EventPersistenceCompletion completion) {
                                            EXPECT_TRUE(completion.outcome.has_value());
                                            {
                                                std::scoped_lock lock{callback_mutex};
                                                ++callbacks;
                                            }
                                            callback_condition.notify_all();
                                        },
                                        {.event_capacity = 1U});
    ASSERT_TRUE(runtime_result);
    auto runtime = std::move(runtime_result).value();
    ASSERT_TRUE(runtime->start());
    ASSERT_TRUE(runtime->submit(request("019f-m506-runtime-0001")));
    writer_pointer->wait_until_entered(1U);
    ASSERT_TRUE(runtime->submit(request("019f-m506-runtime-0002")));

    auto rejected = runtime->submit(request("019f-m506-runtime-0003"));

    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().business_code, "EVENT_WRITE_FAILED");
    EXPECT_EQ(rejected.error().severity, Severity::critical);
    EXPECT_EQ(runtime->snapshot().high_watermark, 1U);
    runtime->request_stop();
    writer_pointer->release();
    ASSERT_TRUE(runtime->join(MonotonicTime::clock::now() + 2s));
    {
        std::unique_lock lock{callback_mutex};
        ASSERT_TRUE(callback_condition.wait_for(lock, 2s, [&] { return callbacks == 2U; }));
    }
    const auto snapshot = runtime->snapshot();
    EXPECT_EQ(snapshot.submitted, 2U);
    EXPECT_EQ(snapshot.completed, 2U);
    EXPECT_EQ(snapshot.rejected, 1U);
    EXPECT_EQ(snapshot.depth, 0U);
}

class FailingWriter final : public IEventTransactionWriter
{
  public:
    Result<EventPersistenceOutcome> persist(const EventPersistenceRequest&) override
    {
        return Result<EventPersistenceOutcome>::failure(make_error(
            "EVENT_WRITE_FAILED", Severity::critical, "test", "storage-test", "persist"));
    }
};

TEST(StorageEventStore, RuntimeIsolatesWriteAndCallbackFailures)
{
    std::condition_variable condition;
    std::mutex mutex;
    bool called = false;
    auto runtime_result = EventPersistenceRuntime::create(
        std::make_unique<FailingWriter>(), [&](EventPersistenceCompletion completion) {
            EXPECT_TRUE(completion.error.has_value());
            {
                std::scoped_lock lock{mutex};
                called = true;
            }
            condition.notify_all();
            throw std::runtime_error{"callback failure"};
        });
    ASSERT_TRUE(runtime_result);
    auto runtime = std::move(runtime_result).value();
    ASSERT_TRUE(runtime->start());
    ASSERT_TRUE(runtime->submit(request("019f-m506-runtime-fail")));
    {
        std::unique_lock lock{mutex};
        ASSERT_TRUE(condition.wait_for(lock, 2s, [&] { return called; }));
    }
    runtime->request_stop();
    ASSERT_TRUE(runtime->join(MonotonicTime::clock::now() + 2s));
    const auto snapshot = runtime->snapshot();
    EXPECT_EQ(snapshot.completed, 1U);
    EXPECT_EQ(snapshot.write_failures, 1U);
    EXPECT_EQ(snapshot.callback_failures, 1U);
}

} // namespace
