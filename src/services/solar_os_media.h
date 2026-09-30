#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SOLAR_OS_MEDIA_RTP_PACKET_MAX 1200U
#define SOLAR_OS_MEDIA_VIDEO_FRAME_MAX (512U * 1024U)

typedef enum {
    SOLAR_OS_MEDIA_TRACK_VIDEO = 0,
    SOLAR_OS_MEDIA_TRACK_AUDIO,
} solar_os_media_track_kind_t;

typedef enum {
    SOLAR_OS_MEDIA_CODEC_JPEG = 0,
    SOLAR_OS_MEDIA_CODEC_L16,
} solar_os_media_codec_t;

typedef struct {
    uint16_t width;
    uint16_t height;
    uint16_t frame_rate;
} solar_os_media_video_format_t;

typedef struct {
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t bits_per_sample;
} solar_os_media_audio_format_t;

typedef struct {
    solar_os_media_track_kind_t kind;
    solar_os_media_codec_t codec;
    uint8_t payload_type;
    uint32_t clock_rate;
    union {
        solar_os_media_video_format_t video;
        solar_os_media_audio_format_t audio;
    } format;
} solar_os_media_track_t;

typedef void (*solar_os_media_release_fn)(void *token);

typedef struct {
    solar_os_media_track_t track;
    uint64_t frame_id;
    uint64_t timestamp_us;
    uint32_t duration_us;
    const uint8_t *data;
    size_t length;
    bool discontinuity;
    bool end_of_stream;
    solar_os_media_release_fn release;
    void *release_token;
} solar_os_media_unit_t;

typedef struct {
    uint64_t origin_us;
    uint32_t origin_timestamp;
    uint32_t clock_rate;
} solar_os_media_clock_t;

bool solar_os_media_track_valid(const solar_os_media_track_t *track);
bool solar_os_media_unit_valid(const solar_os_media_unit_t *unit);
void solar_os_media_unit_release(solar_os_media_unit_t *unit);

esp_err_t solar_os_media_clock_init(solar_os_media_clock_t *clock,
                                    uint64_t origin_us,
                                    uint32_t origin_timestamp,
                                    uint32_t clock_rate);
esp_err_t solar_os_media_clock_map(const solar_os_media_clock_t *clock,
                                   uint64_t timestamp_us,
                                   uint32_t *rtp_timestamp);
