#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

int main()
{
    cv::Mat source = cv::Mat::ones(2, 2, CV_8UC1);
    cv::Mat converted;
    cv::cvtColor(source, converted, cv::COLOR_GRAY2BGR);
    const auto extension_supported = cv::haveImageWriter("smoke.jpg");
    return converted.channels() == 3 && extension_supported ? 0 : 1;
}
