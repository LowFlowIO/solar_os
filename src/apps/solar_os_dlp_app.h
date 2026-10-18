#pragma once
#include "solar_os.h"
#include "solar_os_display.h"
#include "solar_os_stream.h"

typedef struct {
    bool running, suspended;
    char display[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
    char stream[SOLAR_OS_STREAM_ID_MAX];
    uint32_t received_bytes, sent_bytes, presented_frames;
} solar_os_dlp_app_status_t;

extern const solar_os_app_t solar_os_dlp_app;
void solar_os_dlp_app_get_status(solar_os_dlp_app_status_t *status);
