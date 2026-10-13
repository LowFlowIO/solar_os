#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SOLAR_OS_LC709203F_ADDRESS 0x0bU
typedef esp_err_t (*solar_os_lc709203f_read_fn)(void *, uint8_t, uint8_t *,
                                                size_t);
typedef esp_err_t (*solar_os_lc709203f_write_fn)(void *, uint8_t,
                                                 const uint8_t *, size_t);
typedef struct {
    solar_os_lc709203f_read_fn read;
    solar_os_lc709203f_write_fn write;
    void (*delay_ms)(void *, unsigned);
    void *user;
} solar_os_lc709203f_io_t;
typedef struct {
    solar_os_lc709203f_io_t io;
    bool initialized;
    uint16_t version;
    uint8_t apa;
} solar_os_lc709203f_t;
typedef struct {
    uint16_t voltage_mv, ite_tenths;
    uint8_t percent;
} solar_os_lc709203f_sample_t;

uint8_t solar_os_lc709203f_crc8(const uint8_t *data, size_t len);
uint8_t solar_os_lc709203f_capacity_apa(uint32_t capacity_mah);
esp_err_t solar_os_lc709203f_init(solar_os_lc709203f_t *device,
                                  const solar_os_lc709203f_io_t *io,
                                  uint8_t apa, unsigned profile,
                                  int temperature_decic);
esp_err_t solar_os_lc709203f_set_apa(solar_os_lc709203f_t *device, uint8_t apa);
esp_err_t solar_os_lc709203f_read_sample(solar_os_lc709203f_t *device,
                                         solar_os_lc709203f_sample_t *sample);
