#pragma once

#include "esp_err.h"

/* Backend lifecycle hooks publish/unpublish this adapter under the camera
 * lock, so an in-flight stream open also prevents backend removal. */
esp_err_t solar_os_camera_stream_register(const char *name);
esp_err_t solar_os_camera_stream_unregister(const char *name);
