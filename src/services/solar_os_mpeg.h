#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct solar_os_mpeg solar_os_mpeg_t;
typedef struct {
    uint32_t width, height;
    double fps;
    uint32_t sample_rate;
    bool audio;
} solar_os_mpeg_info_t;
typedef struct {
    const uint8_t *y, *cb, *cr;
    uint32_t width, height, y_stride, chroma_stride;
    double time;
} solar_os_mpeg_frame_t;
typedef struct {
    const float *samples; /* Interleaved stereo, including duplicated mono. */
    uint32_t frames, sample_rate;
    double time;
} solar_os_mpeg_audio_t;
typedef bool (*solar_os_mpeg_cancel_t)(void *);

/* Single-owner decoder. Returned planes/samples last until the next decode of
 * that track. MPEG-1 program streams, one video and optional MP2 track only.
 * Compressed track buffers are capped at 256 KiB each; video at 640x480. */
esp_err_t solar_os_mpeg_open(const char *path, solar_os_mpeg_cancel_t cancel, void *user,
                             solar_os_mpeg_t **decoder, char *detail, size_t detail_size);
void solar_os_mpeg_close(solar_os_mpeg_t *decoder);
void solar_os_mpeg_info(const solar_os_mpeg_t *decoder, solar_os_mpeg_info_t *info);
esp_err_t solar_os_mpeg_video(solar_os_mpeg_t *decoder, solar_os_mpeg_frame_t *frame, bool *ended);
esp_err_t solar_os_mpeg_audio(solar_os_mpeg_t *decoder, solar_os_mpeg_audio_t *audio, bool *ended);
const char *solar_os_mpeg_error(const solar_os_mpeg_t *decoder);
/* Fused nearest-neighbour scaling/conversion, with no full-size RGB image.
 * Color is wire-order RGB565; monochrome uses the video luma plane directly.
 * The S3 kernel accelerates color conversion, not MPEG entropy decoding. */
void solar_os_mpeg_raster(const solar_os_mpeg_frame_t *frame, uint8_t *output, uint32_t width,
                          uint32_t height, bool color, bool simd);
bool solar_os_mpeg_simd_available(void);
/* Fast, allocation-free bit-exact kernel check; false selects the fallback. */
bool solar_os_mpeg_simd_selftest(void);
