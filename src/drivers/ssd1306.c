#include "ssd1306.h"

#include <stddef.h>
#include <string.h>

#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "u8x8.h"

#define SSD1306_TILE_HEIGHT 8U

static ssd1306_t *ssd1306_from_u8x8(u8x8_t *u8x8)
{
    if (u8x8 == NULL) {
        return NULL;
    }
    return (ssd1306_t *)((uint8_t *)u8x8 -
                         offsetof(ssd1306_t, u8g2) -
                         offsetof(u8g2_t, u8x8));
}

static void ssd1306_set_error(ssd1306_t *display, esp_err_t err)
{
    if (display != NULL && err != ESP_OK && display->last_error == ESP_OK) {
        display->last_error = err;
    }
}

static uint8_t ssd1306_u8x8_byte_cb(u8x8_t *u8x8,
                                    uint8_t message,
                                    uint8_t arg_int,
                                    void *arg_ptr)
{
    ssd1306_t *display = ssd1306_from_u8x8(u8x8);
    if (display == NULL || display->last_error != ESP_OK) {
        return 0;
    }

    switch (message) {
    case U8X8_MSG_BYTE_INIT:
        return 1; /* The selected transport was probed before display setup. */
    case U8X8_MSG_BYTE_START_TRANSFER:
        display->transfer_len = 0;
        return 1;
    case U8X8_MSG_BYTE_SEND:
        if (arg_int == 0) {
            return 1;
        }
        if (arg_ptr == NULL ||
            display->transfer_len + (size_t)arg_int > sizeof(display->transfer)) {
            ssd1306_set_error(display, ESP_ERR_INVALID_SIZE);
            return 0;
        }
        memcpy(&display->transfer[display->transfer_len], arg_ptr, arg_int);
        display->transfer_len += arg_int;
        return 1;
    case U8X8_MSG_BYTE_END_TRANSFER: {
        if (display->transfer_len == 0) {
            return 1;
        }
        const esp_err_t err = display->io.transmit(display->io.ctx,
                                                    display->transfer,
                                                    display->transfer_len);
        display->transfer_len = 0;
        ssd1306_set_error(display, err);
        return err == ESP_OK;
    }
    case U8X8_MSG_BYTE_SET_DC:
        return 1;
    default:
        return 1;
    }
}

static uint8_t ssd1306_u8x8_gpio_delay_cb(u8x8_t *u8x8,
                                          uint8_t message,
                                          uint8_t arg_int,
                                          void *arg_ptr)
{
    (void)arg_ptr;
    ssd1306_t *display = ssd1306_from_u8x8(u8x8);
    if (display == NULL || display->last_error != ESP_OK) {
        return 0;
    }

    switch (message) {
    case U8X8_MSG_GPIO_AND_DELAY_INIT:
        return 1;
    case U8X8_MSG_DELAY_MILLI:
        if (arg_int <= 10) {
            esp_rom_delay_us((uint32_t)arg_int * 1000U);
        } else {
            vTaskDelay(pdMS_TO_TICKS(arg_int));
        }
        return 1;
    case U8X8_MSG_DELAY_10MICRO:
        esp_rom_delay_us((uint32_t)arg_int * 10U);
        return 1;
    case U8X8_MSG_DELAY_100NANO:
    case U8X8_MSG_DELAY_NANO:
        return 1;
    default:
        return 1;
    }
}

esp_err_t ssd1306_init_with_io(ssd1306_t *display,
                               uint8_t address,
                               ssd1306_controller_t controller,
                               const ssd1306_io_t *io)
{
    if (display == NULL ||
        (address != 0x3c && address != 0x3d) ||
        (controller != SSD1306_CONTROLLER_SSD1306 &&
         controller != SSD1306_CONTROLLER_SH1106) ||
        io == NULL || io->probe == NULL || io->transmit == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const ssd1306_io_t transport = *io;
    memset(display, 0, sizeof(*display));
    display->address = address;
    display->io = transport;
    display->last_error = ESP_OK;

    esp_err_t ret = display->io.probe(display->io.ctx);
    if (ret != ESP_OK) {
        memset(display, 0, sizeof(*display));
        return ret == ESP_ERR_NOT_FOUND ? ESP_ERR_INVALID_RESPONSE : ret;
    }

    u8g2_SetupDisplay(&display->u8g2,
                      controller == SSD1306_CONTROLLER_SH1106
                          ? u8x8_d_sh1106_128x64_noname
                          : u8x8_d_ssd1306_128x64_noname,
                      u8x8_cad_ssd13xx_i2c,
                      ssd1306_u8x8_byte_cb,
                      ssd1306_u8x8_gpio_delay_cb);
    u8g2_SetupBuffer(&display->u8g2,
                     display->buffer,
                     SSD1306_TILE_HEIGHT,
                     u8g2_ll_hvline_vertical_top_lsb,
                     U8G2_R0);
    u8x8_SetI2CAddress(&display->u8g2.u8x8, (uint8_t)(address << 1U));
    u8g2_InitDisplay(&display->u8g2);
    u8g2_SetPowerSave(&display->u8g2, 0);
    if (display->last_error != ESP_OK) {
        ret = display->last_error;
        ssd1306_deinit(display);
        return ret;
    }

    display->initialized = true;
    return ESP_OK;
}

static esp_err_t ssd1306_default_probe(void *ctx)
{
    const ssd1306_t *display = ctx;
    const esp_err_t ret = i2c_bus_init();
    return ret == ESP_OK ? i2c_bus_probe(display->address) : ret;
}

static esp_err_t ssd1306_default_transmit(void *ctx, const uint8_t *data, size_t len)
{
    const ssd1306_t *display = ctx;
    return i2c_bus_transmit(display->address, data, len);
}

esp_err_t ssd1306_init(ssd1306_t *display,
                       uint8_t address,
                       ssd1306_controller_t controller)
{
    const ssd1306_io_t io = {
        .probe = ssd1306_default_probe,
        .transmit = ssd1306_default_transmit,
        .ctx = display,
    };
    return ssd1306_init_with_io(display, address, controller, &io);
}

void ssd1306_deinit(ssd1306_t *display)
{
    if (display == NULL) {
        return;
    }

    if (display->initialized) {
        display->last_error = ESP_OK;
        u8g2_SetPowerSave(&display->u8g2, 1);
    }
    memset(display, 0, sizeof(*display));
}

u8g2_t *ssd1306_get_u8g2(ssd1306_t *display)
{
    return display != NULL ? &display->u8g2 : NULL;
}
