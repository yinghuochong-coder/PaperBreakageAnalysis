#include "paperbreak/event/key_frame_opencv.hpp"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <exception>
#include <limits>
#include <utility>

namespace paperbreak::event
{
namespace
{

Error encode_error(std::string message)
{
    return make_error("EVENT_KEYFRAME_ENCODE_FAILED", Severity::error, std::move(message), "event",
                      "keyframe.encode", true);
}

class OpenCvKeyFrameJpegEncoder final : public IKeyFrameJpegEncoder
{
  public:
    [[nodiscard]] Result<std::vector<std::byte>> encode(
        const camera::FrameView& frame, const KeyFrameJpegEncodeOptions& options) override
    {
        try
        {
            const auto geometry = frame.geometry();
            if (frame.flags().incomplete || geometry.width == 0U || geometry.height == 0U ||
                geometry.stride == 0U || geometry.width > options.maximum_dimension ||
                geometry.height > options.maximum_dimension ||
                frame.bytes().size() > options.maximum_input_bytes ||
                geometry.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
                geometry.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
            {
                return Result<std::vector<std::byte>>::failure(
                    encode_error("关键帧像素布局或输入上限无效"));
            }

            const int type = (frame.pixel_format() == camera::PixelFormat::mono10 ||
                              frame.pixel_format() == camera::PixelFormat::mono12)
                                 ? CV_16UC1
                                 : CV_8UC1;
            const std::size_t bytes_per_pixel = type == CV_16UC1 ? 2U : 1U;
            if (geometry.width > std::numeric_limits<std::size_t>::max() / bytes_per_pixel ||
                geometry.stride < static_cast<std::size_t>(geometry.width) * bytes_per_pixel ||
                geometry.height > std::numeric_limits<std::size_t>::max() / geometry.stride ||
                frame.bytes().size() != static_cast<std::size_t>(geometry.height) * geometry.stride)
            {
                return Result<std::vector<std::byte>>::failure(
                    encode_error("关键帧行跨度或载荷大小无效"));
            }

            cv::Mat source(static_cast<int>(geometry.height), static_cast<int>(geometry.width),
                           type, const_cast<std::byte*>(frame.bytes().data()), geometry.stride);
            cv::Mat image;
            if (frame.pixel_format() == camera::PixelFormat::mono10 ||
                frame.pixel_format() == camera::PixelFormat::mono12)
            {
                const double scale = frame.pixel_format() == camera::PixelFormat::mono10
                                         ? 255.0 / 1023.0
                                         : 255.0 / 4095.0;
                source.convertTo(image, CV_8UC1, scale);
            }
            else if (frame.pixel_format() == camera::PixelFormat::bayer_rg8)
            {
                cv::cvtColor(source, image, cv::COLOR_BayerRG2BGR);
            }
            else
            {
                image = source;
            }

            std::vector<unsigned char> encoded;
            if (!cv::imencode(".jpg", image, encoded,
                              {cv::IMWRITE_JPEG_QUALITY, static_cast<int>(options.jpeg_quality)}) ||
                encoded.empty() || encoded.size() > options.maximum_jpeg_bytes)
            {
                return Result<std::vector<std::byte>>::failure(
                    encode_error("关键帧 JPEG 编码失败或超过字节上限"));
            }
            std::vector<std::byte> result(encoded.size());
            std::transform(encoded.begin(), encoded.end(), result.begin(),
                           [](const unsigned char value) { return static_cast<std::byte>(value); });
            return Result<std::vector<std::byte>>::success(std::move(result));
        }
        catch (const cv::Exception&)
        {
            return Result<std::vector<std::byte>>::failure(
                encode_error("OpenCV 关键帧 JPEG 编码失败"));
        }
        catch (const std::exception&)
        {
            return Result<std::vector<std::byte>>::failure(encode_error("关键帧 JPEG 编码异常"));
        }
    }
};

} // namespace

std::unique_ptr<IKeyFrameJpegEncoder> make_opencv_key_frame_jpeg_encoder()
{
    return std::make_unique<OpenCvKeyFrameJpegEncoder>();
}

} // namespace paperbreak::event
