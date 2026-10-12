#include "inmp441.h"

#include "soc/soc_caps.h"
#include "solar_os_board.h"
#include <string.h>

#define IO_FRAMES 128U
#define STARTUP_FRAMES 4096U /* 2^18 SCK cycles at 64 clocks/frame. */

static uint32_t bounded_timeout(uint32_t timeout) {
    return timeout == 0U || timeout > 1000U ? 1000U : timeout;
}

static void release_channels(solar_os_inmp441_t *d) {
    if (d->rx_enabled)
        (void)i2s_channel_disable(d->rx);
    if (d->tx_enabled)
        (void)i2s_channel_disable(d->tx);
    if (d->rx != NULL)
        (void)i2s_del_channel(d->rx);
    if (d->tx != NULL)
        (void)i2s_del_channel(d->tx);
    d->rx = d->tx = NULL;
    d->rx_enabled = d->tx_enabled = false;
}

esp_err_t solar_os_inmp441_init(solar_os_inmp441_t *d,
                                const solar_os_inmp441_config_t *c) {
    if (d == NULL || c == NULL || c->port < 0 || c->port >= SOC_I2S_NUM ||
        (SOLAR_OS_BOARD_RUNTIME_I2S_PORT_MASK & (1U << c->port)) == 0U ||
        c->slot > 1U || !GPIO_IS_VALID_OUTPUT_GPIO(c->bck) ||
        !GPIO_IS_VALID_OUTPUT_GPIO(c->ws) || !GPIO_IS_VALID_GPIO(c->din) ||
        c->dout < -1 || (c->dout >= 0 && !GPIO_IS_VALID_OUTPUT_GPIO(c->dout)) ||
        c->bck == c->ws || c->bck == c->din || c->ws == c->din ||
        (c->dout >= 0 &&
         (c->dout == c->bck || c->dout == c->ws || c->dout == c->din))) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(d, 0, sizeof(*d));
    d->config = *c;
    d->lock = xSemaphoreCreateMutex();
    return d->lock != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

void solar_os_inmp441_deinit(solar_os_inmp441_t *d) {
    if (d == NULL || d->lock == NULL)
        return;
    release_channels(d);
    vSemaphoreDelete(d->lock);
    memset(d, 0, sizeof(*d));
}

static esp_err_t create_channels(solar_os_inmp441_t *d) {
    if (d->rx != NULL)
        return ESP_OK;
    const solar_os_inmp441_config_t *c = &d->config;
    i2s_chan_config_t channel =
        I2S_CHANNEL_DEFAULT_CONFIG(c->port, I2S_ROLE_MASTER);
    channel.dma_desc_num = 4;
    channel.dma_frame_num = SOLAR_OS_INMP441_BLOCK_FRAMES;
    channel.auto_clear_after_cb = true;
    esp_err_t err =
        i2s_new_channel(&channel, c->dout >= 0 ? &d->tx : NULL, &d->rx);
    if (err != ESP_OK)
        return err;
    i2s_std_config_t standard = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SOLAR_OS_INMP441_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = I2S_GPIO_UNUSED,
                     .bclk = c->bck,
                     .ws = c->ws,
                     .din = c->din,
                     .dout = c->dout},
    };
    err = i2s_channel_init_std_mode(d->rx, &standard);
    if (err == ESP_OK && d->tx != NULL)
        err = i2s_channel_init_std_mode(d->tx, &standard);
    /* In duplex mode keep TX running with cleared DMA buffers during capture.
     * The DAC and speaker receive silence while microphone clocks are running.
     */
    if (err == ESP_OK && d->tx != NULL) {
        err = i2s_channel_enable(d->tx);
        d->tx_enabled = err == ESP_OK;
    }
    if (err != ESP_OK)
        release_channels(d);
    return err;
}

static esp_err_t discard_startup(solar_os_inmp441_t *d) {
    int32_t raw[IO_FRAMES * 2U];
    for (unsigned left = STARTUP_FRAMES; left != 0U; left -= IO_FRAMES) {
        size_t bytes = 0;
        esp_err_t err = i2s_channel_read(d->rx, raw, sizeof(raw), &bytes, 1000);
        if (err != ESP_OK)
            return err;
        if (bytes != sizeof(raw))
            return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t solar_os_inmp441_open(solar_os_inmp441_t *d, bool capture) {
    if (d == NULL || d->lock == NULL)
        return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(d->lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if ((capture && d->capture_open) || (!capture && d->playback_open)) {
        err = ESP_ERR_INVALID_STATE;
    } else if (!capture && d->config.dout < 0) {
        err = ESP_ERR_NOT_SUPPORTED;
    } else {
        err = create_channels(d);
        if (err == ESP_OK && capture) {
            err = i2s_channel_enable(d->rx);
            d->rx_enabled = err == ESP_OK;
            if (err == ESP_OK)
                err = discard_startup(d);
            if (err != ESP_OK && d->rx_enabled) {
                (void)i2s_channel_disable(d->rx);
                d->rx_enabled = false;
            }
        }
        if (err == ESP_OK) {
            if (capture)
                d->capture_open = true;
            else
                d->playback_open = true;
        } else if (!d->capture_open && !d->playback_open) {
            release_channels(d);
        }
    }
    xSemaphoreGive(d->lock);
    return err;
}

void solar_os_inmp441_close(solar_os_inmp441_t *d, bool capture) {
    if (d == NULL || d->lock == NULL)
        return;
    xSemaphoreTake(d->lock, portMAX_DELAY);
    if (capture) {
        d->capture_open = false;
        if (d->rx_enabled)
            (void)i2s_channel_disable(d->rx);
        d->rx_enabled = false;
    } else {
        d->playback_open = false;
        if (d->capture_open && d->tx_enabled) {
            /* Replace queued playback with silence without stopping RX clocks.
             */
            const int32_t silence[IO_FRAMES * 2U] = {0};
            for (unsigned i = 0; i < 8U; ++i) {
                size_t written = 0;
                if (i2s_channel_write(d->tx, silence, sizeof(silence), &written,
                                      1000) != ESP_OK)
                    break;
            }
        }
    }
    if (!d->capture_open && !d->playback_open)
        release_channels(d);
    xSemaphoreGive(d->lock);
}

esp_err_t solar_os_inmp441_read(solar_os_inmp441_t *d, int16_t *samples,
                                size_t frames, uint32_t timeout, size_t *read) {
    if (samples == NULL || read == NULL || frames == 0U)
        return ESP_ERR_INVALID_ARG;
    *read = 0;
    if (d == NULL || !d->capture_open)
        return ESP_ERR_INVALID_STATE;
    int32_t raw[IO_FRAMES * 2U];
    while (*read < frames) {
        const size_t count =
            frames - *read < IO_FRAMES ? frames - *read : IO_FRAMES;
        size_t bytes = 0;
        esp_err_t err =
            i2s_channel_read(d->rx, raw, count * sizeof(int32_t) * 2U, &bytes,
                             bounded_timeout(timeout));
        if (bytes % (sizeof(int32_t) * 2U) != 0U)
            return ESP_ERR_INVALID_SIZE;
        const size_t got = bytes / (sizeof(int32_t) * 2U);
        for (size_t i = 0; i < got; ++i) {
            samples[*read + i] = (int16_t)(raw[i * 2U + d->config.slot] >> 16);
        }
        *read += got;
        if (err != ESP_OK)
            return err;
        if (got != count)
            return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t solar_os_inmp441_write(solar_os_inmp441_t *d, const int16_t *samples,
                                 size_t frames, unsigned channels,
                                 uint8_t volume, uint32_t timeout,
                                 size_t *written) {
    if (samples == NULL || written == NULL || frames == 0U ||
        (channels != 1U && channels != 2U) || volume > 100U)
        return ESP_ERR_INVALID_ARG;
    *written = 0;
    if (d == NULL || !d->playback_open)
        return ESP_ERR_INVALID_STATE;
    int32_t raw[IO_FRAMES * 2U];
    while (*written < frames) {
        const size_t count =
            frames - *written < IO_FRAMES ? frames - *written : IO_FRAMES;
        for (size_t i = 0; i < count; ++i) {
            for (unsigned ch = 0; ch < 2U; ++ch) {
                const int32_t value = samples[(*written + i) * channels +
                                              (channels == 1U ? 0U : ch)];
                raw[i * 2U + ch] = (value * volume / 100) * 65536;
            }
        }
        size_t bytes = 0;
        esp_err_t err =
            i2s_channel_write(d->tx, raw, count * sizeof(int32_t) * 2U, &bytes,
                              bounded_timeout(timeout));
        if (bytes % (sizeof(int32_t) * 2U) != 0U)
            return ESP_ERR_INVALID_SIZE;
        const size_t sent = bytes / (sizeof(int32_t) * 2U);
        *written += sent;
        if (err != ESP_OK)
            return err;
        if (sent != count)
            return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}
