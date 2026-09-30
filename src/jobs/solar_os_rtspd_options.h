#pragma once

#include "solar_os_camera.h"
#include "solar_os_stream.h"

typedef struct {
    bool video;
    char audio[SOLAR_OS_STREAM_ID_MAX];
    solar_os_camera_config_t camera;
    uint8_t fps;
    uint16_t port;
} solar_os_rtspd_options_t;

bool solar_os_rtspd_parse_options(int argc, char **argv,
                                   solar_os_rtspd_options_t *options);
bool solar_os_rtspd_video_due(uint64_t captured_us, uint64_t origin_us,
                               uint64_t last_sent_us, uint8_t fps);
