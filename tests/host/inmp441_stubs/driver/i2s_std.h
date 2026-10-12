#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct fake_channel *i2s_chan_handle_t;
typedef struct {
    int id, role, dma_desc_num, dma_frame_num;
    bool auto_clear_after_cb;
} i2s_chan_config_t;
typedef struct {
    unsigned sample_rate_hz;
} fake_clock_t;
typedef struct {
    unsigned data_bit_width, slot_mode;
} fake_slots_t;
typedef struct {
    int mclk, bclk, ws, din, dout;
} fake_pins_t;
typedef struct {
    fake_clock_t clk_cfg;
    fake_slots_t slot_cfg;
    fake_pins_t gpio_cfg;
} i2s_std_config_t;
#define GPIO_IS_VALID_GPIO(p) ((p) >= 0 && (p) <= 48)
#define GPIO_IS_VALID_OUTPUT_GPIO(p) GPIO_IS_VALID_GPIO(p)
#define I2S_GPIO_UNUSED -1
#define I2S_ROLE_MASTER 1
#define I2S_DATA_BIT_WIDTH_32BIT 32
#define I2S_SLOT_MODE_STEREO 2
#define I2S_CHANNEL_DEFAULT_CONFIG(p, r)                                       \
    ((i2s_chan_config_t){.id = (p), .role = (r)})
#define I2S_STD_CLK_DEFAULT_CONFIG(s) ((fake_clock_t){.sample_rate_hz = (s)})
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(w, m)                              \
    ((fake_slots_t){.data_bit_width = (w), .slot_mode = (m)})
esp_err_t i2s_new_channel(const i2s_chan_config_t *, i2s_chan_handle_t *,
                          i2s_chan_handle_t *);
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t,
                                    const i2s_std_config_t *);
esp_err_t i2s_channel_enable(i2s_chan_handle_t);
esp_err_t i2s_channel_disable(i2s_chan_handle_t);
esp_err_t i2s_del_channel(i2s_chan_handle_t);
esp_err_t i2s_channel_read(i2s_chan_handle_t, void *, size_t, size_t *,
                           uint32_t);
esp_err_t i2s_channel_write(i2s_chan_handle_t, const void *, size_t, size_t *,
                            uint32_t);
