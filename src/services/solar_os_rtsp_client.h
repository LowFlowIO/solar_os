#pragma once

#include "solar_os_rtsp_receiver.h"
#include "solar_os_rtp_jpeg.h"

typedef struct solar_os_rtsp_client solar_os_rtsp_client_t;

typedef struct {
    bool video;
    bool audio;
    /* Called by the audio owner after PCM has been submitted to the output.
     * Valid until run() returns; widgets must outlive that call. */
    void (*samples)(const int16_t *, size_t, uint8_t, void *);
    void *user;
} solar_os_rtsp_client_options_t;

typedef struct {
    bool negotiated, playing, audio_playing;
    bool video, audio;
    uint32_t sample_rate;
    uint8_t channels;
    uint32_t video_frames, video_dropped, audio_dropped;
    esp_err_t error;
} solar_os_rtsp_client_status_t;

esp_err_t solar_os_rtsp_client_create(const char *url,
                                      const solar_os_rtsp_client_options_t *options,
                                      solar_os_rtsp_client_t **client);
/* Single-use blocking, cancellable operation; call on a network worker. Owns
 * RTSP/UDP sockets and an optional audio worker until it returns. */
esp_err_t solar_os_rtsp_client_run(solar_os_rtsp_client_t *client);
void solar_os_rtsp_client_cancel(solar_os_rtsp_client_t *client);
void solar_os_rtsp_client_status(solar_os_rtsp_client_t *client,
                                 solar_os_rtsp_client_status_t *status);
/* One compressed frame, no queue. A successful take leases the assembler
 * buffer until release. Reception drops video while the decoder holds it. */
bool solar_os_rtsp_client_take_video(solar_os_rtsp_client_t *client,
                                    solar_os_rtp_jpeg_frame_t *frame);
void solar_os_rtsp_client_release_video(solar_os_rtsp_client_t *client);
/* Only after run() returned and the final video lease was released. */
esp_err_t solar_os_rtsp_client_destroy(solar_os_rtsp_client_t *client);
