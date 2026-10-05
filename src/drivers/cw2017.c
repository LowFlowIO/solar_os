#include "cw2017.h"

#include <string.h>

/* Cellwise CW2017-DS V1.1; BATINFO is the OEM 80-byte model at 0x10..0x5f. */
#define CW2017_REG_VERSION 0x00U
#define CW2017_REG_VCELL 0x02U
#define CW2017_REG_MODE 0x08U
#define CW2017_REG_SOC_ALERT 0x0bU
#define CW2017_REG_BATINFO 0x10U
#define CW2017_UPDATE_FLAG 0x80U

static bool profile_present(const uint8_t *profile)
{
    bool all_zero = true, all_ff = true;
    for (size_t i = 0; i < CW2017_PROFILE_SIZE; ++i) {
        all_zero &= profile[i] == 0;
        all_ff &= profile[i] == 0xffU;
    }
    return !all_zero && !all_ff;
}

static esp_err_t restart(cw2017_t *device)
{
    /* Datasheet wake/restart sequence. Never change reserved CONFIG bits. */
    const uint8_t reset = 0x30U, normal = 0;
    esp_err_t err = device->io.write(device->io.user, CW2017_REG_MODE, &reset, 1);
    if (err != ESP_OK) return err;
    device->io.delay_ms(device->io.user, 20);
    err = device->io.write(device->io.user, CW2017_REG_MODE, &normal, 1);
    if (err == ESP_OK) device->io.delay_ms(device->io.user, 20);
    return err;
}

esp_err_t cw2017_init(cw2017_t *device, const cw2017_io_t *io)
{
    if (!device || !io || !io->read || !io->write || !io->delay_ms)
        return ESP_ERR_INVALID_ARG;
    memset(device, 0, sizeof(*device));
    device->io = *io;
    uint8_t version = 0, mode = 0;
    esp_err_t err = io->read(io->user, CW2017_REG_VERSION, &version, 1);
    if (err != ESP_OK) return err;
    /* A0 is the power-on version; OEM running firmware reports 0D/0F. */
    if (version != 0xa0U && version != 0x0dU && version != 0x0fU)
        return ESP_ERR_INVALID_RESPONSE;
    err = io->read(io->user, CW2017_REG_MODE, &mode, 1);
    if (err != ESP_OK) return err;
    if ((mode & 0x0fU) != 0) return ESP_ERR_INVALID_RESPONSE;
    if (mode != 0 || version == 0xa0U) {
        err = restart(device);
        if (err != ESP_OK) return err;
    }
    device->version = version;
    device->initialized = true;
    return ESP_OK;
}

esp_err_t cw2017_read_sample(cw2017_t *device, cw2017_sample_t *sample)
{
    if (!device || !device->initialized || !sample) return ESP_ERR_INVALID_STATE;
    uint8_t mode = 0, version = 0, alert = 0, data[4], profile[CW2017_PROFILE_SIZE];
    esp_err_t err = device->io.read(device->io.user, CW2017_REG_MODE, &mode, 1);
    if (err != ESP_OK) return err;
    if (mode != 0) return ESP_ERR_INVALID_STATE;
    err = device->io.read(device->io.user, CW2017_REG_VERSION, &version, 1);
    if (err != ESP_OK) return err;
    err = device->io.read(device->io.user, CW2017_REG_VCELL, data, sizeof(data));
    if (err != ESP_OK) return err;
    err = device->io.read(device->io.user, CW2017_REG_SOC_ALERT, &alert, 1);
    if (err != ESP_OK) return err;
    bool have_profile = false;
    if ((alert & CW2017_UPDATE_FLAG) != 0) {
        err = device->io.read(device->io.user, CW2017_REG_BATINFO,
                              profile, sizeof(profile));
        if (err != ESP_OK) return err;
        have_profile = profile_present(profile);
    }
    const uint16_t vcell = (uint16_t)(((data[0] & 0x3fU) << 8) | data[1]);
    const uint16_t soc = (uint16_t)((data[2] << 8) | data[3]);
    uint16_t percent = (uint16_t)(((uint32_t)soc + 128U) >> 8);
    if (percent > 100U) percent = 100U;
    *sample = (cw2017_sample_t) {
        .voltage_mv = (uint16_t)(((uint32_t)vcell * 5U + 8U) >> 4),
        .soc_raw = soc,
        .percent = (uint8_t)percent,
        .percent_valid = have_profile && data[2] <= 100U &&
            (version == 0x0dU || version == 0x0fU),
    };
    return ESP_OK;
}

esp_err_t cw2017_set_profile(cw2017_t *device, const uint8_t *profile, size_t len)
{
    if (!device || !device->initialized) return ESP_ERR_INVALID_STATE;
    if (!profile || len != CW2017_PROFILE_SIZE || !profile_present(profile))
        return ESP_ERR_INVALID_ARG;
    uint8_t alert = 0, check[CW2017_PROFILE_SIZE];
    esp_err_t err = device->io.read(device->io.user, CW2017_REG_SOC_ALERT, &alert, 1);
    if (err != ESP_OK) return err;
    /* Invalidate first so a failed or interrupted upload cannot publish bad SOC. */
    const uint8_t invalid = (uint8_t)(alert & ~CW2017_UPDATE_FLAG);
    err = device->io.write(device->io.user, CW2017_REG_SOC_ALERT, &invalid, 1);
    if (err != ESP_OK) return err;
    for (size_t i = 0; i < len; ++i) {
        err = device->io.write(device->io.user, (uint8_t)(CW2017_REG_BATINFO + i),
                               &profile[i], 1);
        if (err != ESP_OK) return err;
    }
    err = device->io.read(device->io.user, CW2017_REG_BATINFO, check, sizeof(check));
    if (err != ESP_OK) return err;
    if (memcmp(profile, check, len) != 0) return ESP_ERR_INVALID_RESPONSE;
    alert |= CW2017_UPDATE_FLAG;
    err = device->io.write(device->io.user, CW2017_REG_SOC_ALERT, &alert, 1);
    return err == ESP_OK ? restart(device) : err;
}
