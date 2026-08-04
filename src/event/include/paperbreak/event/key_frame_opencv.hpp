#pragma once

#include "paperbreak/event/key_frame.hpp"

#include <memory>

namespace paperbreak::event
{

/// Creates the approved OpenCV-backed JPEG adapter. Invoke it only through the JPEG worker.
[[nodiscard]] std::unique_ptr<IKeyFrameJpegEncoder> make_opencv_key_frame_jpeg_encoder();

} // namespace paperbreak::event
