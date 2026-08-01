#pragma once

#include "paperbreak/camera/camera.hpp"

#include <memory>

namespace paperbreak::camera::hikrobot
{

/// Creates the production MVS-backed camera provider. No SDK type crosses this boundary.
[[nodiscard]] std::unique_ptr<ICameraProvider> create_hikrobot_camera_provider();

} // namespace paperbreak::camera::hikrobot
