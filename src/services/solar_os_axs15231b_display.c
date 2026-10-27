#include "solar_os_axs15231b_display.h"

#include <string.h>

#include "driver/spi_common.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lcd_axs15231b.h"
#include "solar_os_board.h"
#include "solar_os_board_display.h"
#include "solar_os_display.h"

/* Board overrides come from the manifest [defines] table. Defaults match the
 * Guition JC3248W535 (native 320x480 portrait panel shown as 480x320). */
#ifndef SOLAR_OS_BOARD_DISPLAY_SPI_CLOCK_HZ
#define SOLAR_OS_BOARD_DISPLAY_SPI_CLOCK_HZ 40000000U
#endif
#ifndef SOLAR_OS_BOARD_DISPLAY_QSPI_HOST
#define SOLAR_OS_BOARD_DISPLAY_QSPI_HOST SPI2_HOST
#endif
#ifndef SOLAR_OS_BOARD_DISPLAY_NATIVE_WIDTH
#define SOLAR_OS_BOARD_DISPLAY_NATIVE_WIDTH 320
#endif
#ifndef SOLAR_OS_BOARD_DISPLAY_NATIVE_HEIGHT
#define SOLAR_OS_BOARD_DISPLAY_NATIVE_HEIGHT 480
#endif
#ifndef SOLAR_OS_BOARD_DISPLAY_MADCTL
#define SOLAR_OS_BOARD_DISPLAY_MADCTL 0x08
#endif
#ifndef SOLAR_OS_BOARD_DISPLAY_U8G2_ROTATION
#define SOLAR_OS_BOARD_DISPLAY_U8G2_ROTATION U8G2_R1
#endif
#ifndef SOLAR_OS_BOARD_DISPLAY_INVERT_COLOR
#define SOLAR_OS_BOARD_DISPLAY_INVERT_COLOR 0
#endif
#ifndef SOLAR_OS_BOARD_DISPLAY_FULL_FRAME
#define SOLAR_OS_BOARD_DISPLAY_FULL_FRAME 0
#endif
/* Transport defaults follow the user's working ESPHome mipi_spi setup:
 * SPI mode 0 with CS driven as a GPIO around each transfer. */
#ifndef SOLAR_OS_BOARD_DISPLAY_SPI_MODE
#define SOLAR_OS_BOARD_DISPLAY_SPI_MODE 0
#endif
#ifndef SOLAR_OS_BOARD_DISPLAY_SOFTWARE_CS
#define SOLAR_OS_BOARD_DISPLAY_SOFTWARE_CS 1
#endif
#ifndef SOLAR_OS_BOARD_DISPLAY_VENDOR_INIT
#define SOLAR_OS_BOARD_DISPLAY_VENDOR_INIT 0
#endif
#ifndef SOLAR_OS_BOARD_DISPLAY_SELF_TEST
#define SOLAR_OS_BOARD_DISPLAY_SELF_TEST 0
#endif
#ifndef SOLAR_OS_BOARD_LCD_BACKLIGHT_PWM_FREQ_HZ
#define SOLAR_OS_BOARD_LCD_BACKLIGHT_PWM_FREQ_HZ 20000U
#endif
#ifndef SOLAR_OS_BOARD_LCD_RESET_LOW_MS
#define SOLAR_OS_BOARD_LCD_RESET_LOW_MS 150U
#endif

typedef struct {
    bool active;
    bool primary;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    lcd_axs15231b_t driver;
    solar_os_board_display_t display;
} axs_device_t;

static axs_device_t *device;

typedef struct {
    int clk;
    int d[4];
    int cs;
    int reset;
    int backlight;
    bool active_high;
    bool pwm;
} axs_pins_t;

static bool role_is(const solar_os_expansion_binding_t *binding, const char *role)
{
    return binding != NULL && strcmp(binding->role, role) == 0;
}

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t count, axs_pins_t *pins)
{
    static const char *const data_roles[4] = {"d0", "d1", "d2", "d3"};
    pins->clk = -1;
    pins->cs = -1;
    pins->reset = -1;
    pins->backlight = -1;
    pins->active_high = true;
    pins->pwm = false;
    for (size_t i = 0; i < 4U; i++) {
        pins->d[i] = -1;
    }
    for (size_t i = 0; i < count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        if (binding->kind == SOLAR_OS_EXPANSION_BINDING_GPIO) {
            bool matched = false;
            for (size_t lane = 0; lane < 4U; lane++) {
                if (role_is(binding, data_roles[lane]) && pins->d[lane] < 0) {
                    pins->d[lane] = binding->value;
                    matched = true;
                }
            }
            if (matched) continue;
            if (role_is(binding, "clk") && pins->clk < 0) {
                pins->clk = binding->value;
            } else if (role_is(binding, "cs") && pins->cs < 0) {
                pins->cs = binding->value;
            } else if (role_is(binding, "reset") && pins->reset < 0) {
                pins->reset = binding->value;
            } else if (role_is(binding, "bl") && pins->backlight < 0) {
                pins->backlight = binding->value;
            } else {
                return ESP_ERR_INVALID_ARG;
            }
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER &&
                   role_is(binding, "active")) {
            pins->active_high = binding->value != 0;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER &&
                   role_is(binding, "pwm")) {
            pins->pwm = binding->value != 0;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (pins->clk < 0 || pins->cs < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t lane = 0; lane < 4U; lane++) {
        if (pins->d[lane] < 0) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

static esp_err_t runtime_ready(solar_os_board_display_t *display)
{
    return display != NULL && display->driver != NULL ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t resume(solar_os_board_display_t *display)
{
    const esp_err_t ret = display != NULL && display->driver != NULL ?
        lcd_axs15231b_resume(display->driver) : ESP_ERR_INVALID_STATE;
    if (display != NULL) {
        display->ready = ret == ESP_OK;
    }
    return ret;
}

static void deinit(solar_os_board_display_t *display)
{
    if (display != NULL && display->driver != NULL) {
        lcd_axs15231b_deinit(display->driver);
        display->ready = false;
    }
}

static esp_err_t set_power_save(solar_os_board_display_t *display, bool enabled)
{
    return display != NULL && display->driver != NULL ?
        lcd_axs15231b_set_power_save(display->driver, enabled) : ESP_ERR_INVALID_STATE;
}

static bool brightness_supported(const solar_os_board_display_t *display)
{
    return display != NULL && lcd_axs15231b_backlight_supported(display->driver);
}

static esp_err_t get_brightness(const solar_os_board_display_t *display, uint8_t *percent)
{
    return display != NULL ? lcd_axs15231b_get_backlight(display->driver, percent) :
        ESP_ERR_INVALID_STATE;
}

static esp_err_t set_brightness(solar_os_board_display_t *display, uint8_t percent)
{
    return display != NULL ? lcd_axs15231b_set_backlight(display->driver, percent) :
        ESP_ERR_INVALID_STATE;
}

static esp_err_t set_colors(solar_os_board_display_t *display, uint32_t foreground,
                            uint32_t background)
{
    return display != NULL ?
        lcd_axs15231b_set_colors(display->driver, foreground, background) :
        ESP_ERR_INVALID_STATE;
}

static esp_err_t present_surface(solar_os_board_display_t *display,
                                 const solar_os_display_surface_t *surface)
{
    return display != NULL ? lcd_axs15231b_present_surface(display->driver, surface) :
        ESP_ERR_INVALID_STATE;
}

static esp_err_t present_frame(solar_os_board_display_t *display,
                               const solar_os_display_raster_t *frame)
{
    return display != NULL ? lcd_axs15231b_present_frame(display->driver, frame) :
        ESP_ERR_INVALID_STATE;
}

static esp_err_t present_auxiliary_surface(void *context,
                                           const solar_os_display_surface_t *surface)
{
    return lcd_axs15231b_present_surface((lcd_axs15231b_t *)context, surface);
}

static esp_err_t present_auxiliary_frame(void *context,
                                         const solar_os_display_raster_t *frame)
{
    return lcd_axs15231b_present_frame((lcd_axs15231b_t *)context, frame);
}

static const solar_os_board_display_ops_t display_ops = {
    .runtime_ready = runtime_ready,
    .resume = resume,
    .deinit = deinit,
    .set_power_save = set_power_save,
    .brightness_supported = brightness_supported,
    .get_brightness = get_brightness,
    .set_brightness = set_brightness,
    .set_colors = set_colors,
    .present_surface = present_surface,
    .present_frame = present_frame,
};

static esp_err_t register_auxiliary(axs_device_t *attached)
{
    solar_os_display_target_t target = {0};
    strlcpy(target.name, attached->name, sizeof(target.name));
    strlcpy(target.source, "expansion", sizeof(target.source));
    strlcpy(target.driver, attached->display.driver_name, sizeof(target.driver));
    strlcpy(target.controller, attached->display.controller, sizeof(target.controller));
    strlcpy(target.role, "aux", sizeof(target.role));
    target.width = attached->display.width;
    target.height = attached->display.height;
    target.ready = true;
    target.brightness_supported = brightness_supported(&attached->display);
    target.surface_formats = attached->display.surface_formats;
    target.frame_formats = attached->display.frame_formats;
    target.preferred_stream_fps = attached->display.preferred_stream_fps;
    target.max_stream_pixels_per_second = attached->display.max_stream_pixels_per_second;
    target.u8g2 = attached->display.u8g2;
    target.surface_context = &attached->driver;
    target.present_surface = present_auxiliary_surface;
    target.frame_context = &attached->driver;
    target.present_frame = present_auxiliary_frame;
    return solar_os_display_register_target(&target);
}

static esp_err_t attach(const char *name, const solar_os_expansion_binding_t *bindings,
                        size_t binding_count)
{
    if (device != NULL || name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    axs_pins_t pins;
    ESP_RETURN_ON_ERROR(parse_bindings(bindings, binding_count, &pins), "axs15231b",
                        "invalid bindings");
    device = heap_caps_calloc(1, sizeof(*device), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (device == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const lcd_axs15231b_config_t config = {
        .spi_host = SOLAR_OS_BOARD_DISPLAY_QSPI_HOST,
        .sclk_pin = pins.clk,
        .data0_pin = pins.d[0],
        .data1_pin = pins.d[1],
        .data2_pin = pins.d[2],
        .data3_pin = pins.d[3],
        .cs_pin = pins.cs,
        .reset_pin = pins.reset,
        .backlight_pin = pins.backlight,
        .spi_clock_hz = SOLAR_OS_BOARD_DISPLAY_SPI_CLOCK_HZ,
        .backlight_pwm_hz = SOLAR_OS_BOARD_LCD_BACKLIGHT_PWM_FREQ_HZ,
        .reset_low_ms = SOLAR_OS_BOARD_LCD_RESET_LOW_MS,
        .width = SOLAR_OS_BOARD_DISPLAY_NATIVE_WIDTH,
        .height = SOLAR_OS_BOARD_DISPLAY_NATIVE_HEIGHT,
        .madctl = SOLAR_OS_BOARD_DISPLAY_MADCTL,
        .invert_color = SOLAR_OS_BOARD_DISPLAY_INVERT_COLOR != 0,
        .backlight_active_high = pins.active_high,
        .backlight_pwm = pins.pwm,
        .full_frame_only = SOLAR_OS_BOARD_DISPLAY_FULL_FRAME != 0,
        .spi_mode = (uint8_t)SOLAR_OS_BOARD_DISPLAY_SPI_MODE,
        .software_cs = SOLAR_OS_BOARD_DISPLAY_SOFTWARE_CS != 0,
        .vendor_init = SOLAR_OS_BOARD_DISPLAY_VENDOR_INIT != 0,
        .self_test = SOLAR_OS_BOARD_DISPLAY_SELF_TEST != 0,
        .rotation = SOLAR_OS_BOARD_DISPLAY_U8G2_ROTATION,
    };
    esp_err_t ret = lcd_axs15231b_init(&device->driver, &config);
    if (ret != ESP_OK) {
        /* lcd_axs15231b_init releases the bus and turns the backlight off. */
        ESP_LOGE("axs15231b", "display %s init failed: %s", name, esp_err_to_name(ret));
        heap_caps_free(device);
        device = NULL;
        return ret;
    }
    strlcpy(device->name, name, sizeof(device->name));
    u8g2_t *const u8g2 = lcd_axs15231b_get_u8g2(&device->driver);
    device->display = (solar_os_board_display_t) {
        .ops = &display_ops,
        .driver = &device->driver,
        .driver_name = "axs15231b",
        .u8g2 = u8g2,
        .controller = "AXS15231B",
        .width = u8g2_GetDisplayWidth(u8g2),
        .height = u8g2_GetDisplayHeight(u8g2),
        .surface_formats = SOLAR_OS_DISPLAY_FORMAT_INDEX8_BIT,
        .frame_formats = SOLAR_OS_DISPLAY_FORMAT_INDEX2_BIT |
            SOLAR_OS_DISPLAY_FORMAT_INDEX8_BIT | SOLAR_OS_DISPLAY_FORMAT_RGB565_BIT,
        .preferred_stream_fps = 25,
        .max_stream_pixels_per_second = 1600000U,
        .ready = true,
    };
    device->primary = solar_os_display_target_is_board_primary(name);
    ret = device->primary ? solar_os_board_display_register_primary(&device->display) :
        register_auxiliary(device);
    if (ret != ESP_OK) {
        lcd_axs15231b_deinit(&device->driver);
        heap_caps_free(device);
        device = NULL;
        return ret;
    }
    device->active = true;
    return ESP_OK;
}

static esp_err_t detach(const char *name)
{
    if (device == NULL || !device->active || name == NULL ||
        strcmp(device->name, name) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    const esp_err_t ret = device->primary ?
        solar_os_board_display_unregister_primary(&device->display) :
        solar_os_display_unregister_target(name);
    if (ret != ESP_OK) {
        return ret;
    }
    lcd_axs15231b_deinit(&device->driver);
    heap_caps_free(device);
    device = NULL;
    return ESP_OK;
}

static const int bool_values[] = {0, 1};
static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "clk", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "clk", .required = true},
    {.key = "d0", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "d0", .required = true},
    {.key = "d1", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "d1", .required = true},
    {.key = "d2", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "d2", .required = true},
    {.key = "d3", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "d3", .required = true},
    {.key = "cs", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "cs", .required = true},
    {.key = "reset", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "reset"},
    {.key = "bl", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "bl"},
    {.key = "active", .value_hint = "0|1", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "active", .allowed_values = bool_values, .allowed_value_count = 2},
    {.key = "pwm", .value_hint = "0|1", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "pwm", .allowed_values = bool_values, .allowed_value_count = 2},
};

const solar_os_expansion_driver_t solar_os_axs15231b_expansion_driver = {
    .name = "axs15231b",
    .category = SOLAR_OS_EXPANSION_CATEGORY_DISPLAY,
    .summary = "320x480 QSPI color TFT",
    .required_capabilities = SOLAR_OS_BOARD_CAP_GFX | SOLAR_OS_BOARD_CAP_EXPANSION_GPIO,
    .early = true,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = attach,
    .detach = detach,
};
