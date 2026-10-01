#include "solar_os_media.h"

bool solar_os_media_track_valid(const solar_os_media_track_t *track)
{
    if (track == NULL || track->payload_type < 96U ||
        track->payload_type > 127U || track->clock_rate == 0U) {
        return false;
    }
    if (track->kind == SOLAR_OS_MEDIA_TRACK_VIDEO &&
        track->codec == SOLAR_OS_MEDIA_CODEC_JPEG) {
        return track->clock_rate == 90000U &&
            track->format.video.width > 0U &&
            track->format.video.height > 0U &&
            track->format.video.frame_rate > 0U;
    }
    if (track->kind == SOLAR_OS_MEDIA_TRACK_AUDIO &&
        track->codec == SOLAR_OS_MEDIA_CODEC_L16) {
        return track->clock_rate == track->format.audio.sample_rate &&
            track->format.audio.channels > 0U &&
            track->format.audio.channels <= 2U &&
            track->format.audio.bits_per_sample == 16U;
    }
    return false;
}

bool solar_os_media_unit_valid(const solar_os_media_unit_t *unit)
{
    return unit != NULL && solar_os_media_track_valid(&unit->track) &&
        unit->data != NULL && unit->length > 0U &&
        unit->length <= SOLAR_OS_MEDIA_VIDEO_FRAME_MAX;
}

void solar_os_media_unit_release(solar_os_media_unit_t *unit)
{
    if (unit == NULL) {
        return;
    }
    solar_os_media_release_fn release = unit->release;
    void *token = unit->release_token;
    unit->data = NULL;
    unit->length = 0U;
    unit->release = NULL;
    unit->release_token = NULL;
    if (release != NULL) {
        release(token);
    }
}

esp_err_t solar_os_media_clock_init(solar_os_media_clock_t *clock,
                                    uint64_t origin_us,
                                    uint32_t origin_timestamp,
                                    uint32_t clock_rate)
{
    if (clock == NULL || clock_rate == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    *clock = (solar_os_media_clock_t) {
        .origin_us = origin_us,
        .origin_timestamp = origin_timestamp,
        .clock_rate = clock_rate,
    };
    return ESP_OK;
}

esp_err_t solar_os_media_clock_map(const solar_os_media_clock_t *clock,
                                   uint64_t timestamp_us,
                                   uint32_t *rtp_timestamp)
{
    if (clock == NULL || rtp_timestamp == NULL || clock->clock_rate == 0U ||
        timestamp_us < clock->origin_us) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint64_t delta_us = timestamp_us - clock->origin_us;
    const uint64_t seconds = delta_us / 1000000U;
    const uint64_t remainder_us = delta_us % 1000000U;
    const uint64_t ticks = seconds * clock->clock_rate +
        (remainder_us * clock->clock_rate) / 1000000U;
    *rtp_timestamp = clock->origin_timestamp + (uint32_t)ticks;
    return ESP_OK;
}
