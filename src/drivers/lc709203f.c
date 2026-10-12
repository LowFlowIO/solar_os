#include "lc709203f.h"
#include <string.h>

/* onsemi LC709203F/D Rev.16: SMBus-style CRC-8, polynomial x^8+x^2+x+1.
 * The packet checksum includes both address bytes, command and little-endian
 * word. */
uint8_t solar_os_lc709203f_crc8(const uint8_t *data, size_t len) {
    uint8_t crc = 0;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (unsigned b = 0; b < 8; ++b)
            crc = (uint8_t)((crc << 1) ^ ((crc & 0x80U) ? 0x07U : 0U));
    }
    return crc;
}

uint8_t solar_os_lc709203f_capacity_apa(uint32_t capacity) {
    /* Table 7, Type-01/Type-03 cells. Choose the nearest characterized
     * capacity. */
    static const uint32_t mah[] = {100, 200, 500, 1000, 2000, 3000};
    static const uint8_t apa[] = {0x08, 0x0b, 0x10, 0x19, 0x2d, 0x36};
    unsigned best = 0;
    uint32_t distance =
        capacity > mah[0] ? capacity - mah[0] : mah[0] - capacity;
    for (unsigned i = 1; i < sizeof(mah) / sizeof(mah[0]); ++i) {
        uint32_t d = capacity > mah[i] ? capacity - mah[i] : mah[i] - capacity;
        if (d < distance) {
            best = i;
            distance = d;
        }
    }
    return apa[best];
}

static esp_err_t read_word(solar_os_lc709203f_t *d, uint8_t reg,
                           uint16_t *value) {
    uint8_t packet[] = {0x16, reg, 0x17, 0, 0, 0};
    esp_err_t err = d->io.read(d->io.user, reg, packet + 3, 3);
    if (err != ESP_OK)
        return err;
    if (solar_os_lc709203f_crc8(packet, 5) != packet[5])
        return ESP_ERR_INVALID_CRC;
    *value = (uint16_t)(packet[3] | ((uint16_t)packet[4] << 8));
    return ESP_OK;
}

static esp_err_t write_word(solar_os_lc709203f_t *d, uint8_t reg,
                            uint16_t value) {
    uint8_t packet[] = {0x16, reg, (uint8_t)value, (uint8_t)(value >> 8), 0};
    packet[4] = solar_os_lc709203f_crc8(packet, 4);
    return d->io.write(d->io.user, reg, packet + 2, 3);
}

esp_err_t solar_os_lc709203f_set_apa(solar_os_lc709203f_t *d, uint8_t apa) {
    if (d == NULL || !d->initialized || apa == 0)
        return ESP_ERR_INVALID_ARG;
    if (apa == d->apa)
        return ESP_OK;
    esp_err_t err = write_word(d, 0x0b, apa);
    if (err == ESP_OK)
        d->apa = apa;
    return err;
}

esp_err_t solar_os_lc709203f_init(solar_os_lc709203f_t *d,
                                  const solar_os_lc709203f_io_t *io,
                                  uint8_t apa, unsigned profile,
                                  int temperature) {
    if (d == NULL || io == NULL || io->read == NULL || io->write == NULL ||
        io->delay_ms == NULL || apa == 0 || profile > 1 || temperature < -200 ||
        temperature > 600)
        return ESP_ERR_INVALID_ARG;
    memset(d, 0, sizeof(*d));
    d->io = *io;
    /* TINIT is at most 90 ms after power-on or a profile change (Table 4). */
    io->delay_ms(io->user, 100);
    esp_err_t err = read_word(d, 0x11, &d->version);
    if (err == ESP_OK && (d->version == 0 || d->version == 0xffff))
        err = ESP_ERR_INVALID_RESPONSE;
    /* The first Operational command also supplies the wakeup SDA pulse
     * (Fig.21). */
    if (err == ESP_OK)
        err = write_word(d, 0x15, 1);
    if (err == ESP_OK)
        err = write_word(d, 0x15, 1);
    uint16_t current_profile = 0;
    if (err == ESP_OK)
        err = read_word(d, 0x12, &current_profile);
    if (err == ESP_OK && current_profile != profile) {
        err = write_word(d, 0x12, (uint16_t)profile);
        if (err == ESP_OK)
            io->delay_ms(io->user, 100);
        if (err == ESP_OK)
            err = write_word(d, 0x15, 1);
        if (err == ESP_OK)
            err = write_word(d, 0x15, 1);
    }
    /* A profile change resets configuration; apply these after initialization.
     */
    if (err == ESP_OK)
        err = write_word(d, 0x0b, apa);
    if (err == ESP_OK)
        err = write_word(d, 0x0a, 0); /* Automatic charge/discharge tracking. */
    if (err == ESP_OK)
        err = write_word(
            d, 0x16,
            0); /* Host-supplied temperature, no thermistor required. */
    if (err == ESP_OK)
        err = write_word(d, 0x08, (uint16_t)(2732 + temperature));
    if (err != ESP_OK) {
        memset(d, 0, sizeof(*d));
        return err;
    }
    d->apa = apa;
    d->initialized = true;
    return ESP_OK;
}

esp_err_t solar_os_lc709203f_read_sample(solar_os_lc709203f_t *d,
                                         solar_os_lc709203f_sample_t *sample) {
    if (d == NULL || !d->initialized || sample == NULL)
        return ESP_ERR_INVALID_STATE;
    uint16_t voltage, ite, power;
    esp_err_t err = read_word(d, 0x15, &power);
    /* WLCSP variants can auto-sleep after initialization. Recover before
     * sampling. */
    if (err == ESP_OK && power == 2) {
        err = write_word(d, 0x15, 1);
        if (err == ESP_OK)
            err = write_word(d, 0x15, 1);
    } else if (err == ESP_OK && power != 1) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err == ESP_OK)
        err = read_word(d, 0x09, &voltage);
    if (err == ESP_OK)
        err = read_word(d, 0x0f, &ite);
    if (err != ESP_OK)
        return err;
    if (voltage < 2500 || voltage > 4500 || ite > 1000)
        return ESP_ERR_INVALID_RESPONSE;
    *sample =
        (solar_os_lc709203f_sample_t){.voltage_mv = voltage,
                                      .ite_tenths = ite,
                                      .percent = (uint8_t)((ite + 5U) / 10U)};
    return ESP_OK;
}
