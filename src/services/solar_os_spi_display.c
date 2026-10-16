#include "solar_os_spi_display.h"
#include "freertos/FreeRTOS.h"
#include "solar_os_display.h"
#include "solar_os_display_link.h"
#include <string.h>

typedef struct {
  bool reserved;
  char name[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
  solar_os_display_link_t *driver;
} device_t;
static device_t devices[2];
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

esp_err_t solar_os_spi_display_attach(const char *name,
                                      const solar_os_expansion_binding_t *b,
                                      size_t n) {
  if (!name || !*name ||
      strnlen(name, SOLAR_OS_DISPLAY_TARGET_NAME_MAX) >=
          SOLAR_OS_DISPLAY_TARGET_NAME_MAX ||
      !b)
    return ESP_ERR_INVALID_ARG;
  solar_os_display_link_config_t c = {.cs = -1,
                                      .ready = -1,
                                      .reset = -1,
                                      .reset_ms = 10,
                                      .boot_ms = 5000,
                                      .clock_hz = 10000000};
  char bus[SOLAR_OS_EXPANSION_TARGET_MAX] = {0};
  for (size_t i = 0; i < n; i++) {
    if (b[i].kind == SOLAR_OS_EXPANSION_BINDING_SPI_BUS)
      strlcpy(bus, b[i].target, sizeof(bus));
    else if (b[i].kind == SOLAR_OS_EXPANSION_BINDING_SPI_CS)
      c.cs = b[i].value;
    else if (b[i].kind == SOLAR_OS_EXPANSION_BINDING_GPIO &&
             !strcmp(b[i].role, "ready"))
      c.ready = b[i].value;
    else if (b[i].kind == SOLAR_OS_EXPANSION_BINDING_GPIO &&
             !strcmp(b[i].role, "reset"))
      c.reset = b[i].value;
    else if (b[i].kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER) {
      if (!strcmp(b[i].role, "clock"))
        c.clock_hz = (uint32_t)b[i].value * 1000;
      else if (!strcmp(b[i].role, "reset-ms"))
        c.reset_ms = (uint32_t)b[i].value;
      else if (!strcmp(b[i].role, "boot-ms"))
        c.boot_ms = (uint32_t)b[i].value;
      else if (!strcmp(b[i].role, "reset-active"))
        c.reset_active_high = b[i].value != 0;
      else
        return ESP_ERR_INVALID_ARG;
    } else
      return ESP_ERR_INVALID_ARG;
  }
  solar_os_expansion_spi_bus_t descriptor;
  if (!solar_os_expansion_find_spi_bus(bus, &descriptor, NULL) ||
      descriptor.miso_pin < 0 || descriptor.max_transfer_size < 256 ||
      !solar_os_expansion_spi_cs_allowed(bus, c.cs) || c.clock_hz < 100000 ||
      !c.boot_ms || c.boot_ms > 60000 || !c.reset_ms || c.reset_ms > 1000)
    return ESP_ERR_INVALID_ARG;
  c.bus = bus;
  portENTER_CRITICAL(&lock);
  device_t *slot = NULL;
  bool duplicate = false;
  for (size_t i = 0; i < 2; i++) {
    if (devices[i].reserved && !strcmp(devices[i].name, name))
      duplicate = true;
    if (!devices[i].reserved)
      slot = &devices[i];
  }
  if (slot && !duplicate) {
    slot->reserved = true;
    strlcpy(slot->name, name, sizeof(slot->name));
  }
  portEXIT_CRITICAL(&lock);
  if (duplicate)
    return ESP_ERR_INVALID_STATE;
  if (!slot)
    return ESP_ERR_NO_MEM;
  solar_os_display_link_t *driver = NULL;
  esp_err_t e = solar_os_display_link_create(&c, &driver);
  if (e == ESP_OK) {
    const sdlp_info_t *info = solar_os_display_link_info(driver);
    solar_os_display_target_t t = {0};
    strlcpy(t.name, name, sizeof(t.name));
    strlcpy(t.source, "expansion", sizeof(t.source));
    strlcpy(t.driver, "spi-display", sizeof(t.driver));
    strlcpy(t.controller, "SDLP v1", sizeof(t.controller));
    strlcpy(t.role, "aux", sizeof(t.role));
    t.width = info->width;
    t.height = info->height;
    t.ready = true;
    t.black_is_one = true;
    t.u8g2 = solar_os_display_link_u8g2(driver);
    t.frame_formats =
        SOLAR_OS_DISPLAY_FORMAT_MONO1_BIT | SOLAR_OS_DISPLAY_FORMAT_INDEX2_BIT |
        SOLAR_OS_DISPLAY_FORMAT_INDEX8_BIT | SOLAR_OS_DISPLAY_FORMAT_RGB565_BIT;
    t.surface_formats = SOLAR_OS_DISPLAY_FORMAT_INDEX8_BIT;
    t.surface_context = driver;
    t.present_surface = solar_os_display_link_surface;
    t.frame_context = driver;
    t.present_frame = solar_os_display_link_frame;
    t.power_state_context = driver;
    t.present_power_state = solar_os_display_link_power_state;
    /* Conservative useful pixel budget: both request and response cells,
     * and one minimum CS-high gap per cell, at the selected wire depth. */
    uint32_t hz =
        c.clock_hz < info->max_clock_hz ? c.clock_hz : info->max_clock_hz;
    uint64_t cell_us = UINT64_C(2048000000) / hz + info->min_gap_us + 100;
    uint32_t bytes_sec = (uint32_t)(UINT64_C(208000000) / (2 * cell_us));
    unsigned bits[] = {1, 2, 4, 8, 16};
    uint32_t pps = bytes_sec * 8 / bits[info->preferred_format];
    t.max_stream_pixels_per_second = pps ? pps : 1;
    t.preferred_stream_fps = (uint16_t)(pps / ((uint32_t)t.width * t.height));
    if (!t.preferred_stream_fps)
      t.preferred_stream_fps = 1;
    e = solar_os_display_register_target(&t);
  }
  if (e != ESP_OK)
    solar_os_display_link_destroy(driver);
  portENTER_CRITICAL(&lock);
  if (e == ESP_OK)
    slot->driver = driver;
  else
    memset(slot, 0, sizeof(*slot));
  portEXIT_CRITICAL(&lock);
  return e;
}
esp_err_t solar_os_spi_display_detach(const char *name) {
  if (!name)
    return ESP_ERR_INVALID_ARG;
  device_t *slot = NULL;
  solar_os_display_link_t *driver = NULL;
  portENTER_CRITICAL(&lock);
  for (size_t i = 0; i < 2; i++)
    if (devices[i].reserved && !strcmp(devices[i].name, name)) {
      slot = &devices[i];
      driver = slot->driver;
      break;
    }
  portEXIT_CRITICAL(&lock);
  if (!slot || !driver)
    return ESP_ERR_NOT_FOUND;
  esp_err_t e = solar_os_display_unregister_target(name);
  if (e != ESP_OK)
    return e;
  solar_os_display_link_destroy(driver);
  portENTER_CRITICAL(&lock);
  memset(slot, 0, sizeof(*slot));
  portEXIT_CRITICAL(&lock);
  return ESP_OK;
}
