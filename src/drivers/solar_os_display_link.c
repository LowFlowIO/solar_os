#include "solar_os_display_link.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "solar_os_buses.h"
#include "solar_os_memory.h"
#include <stddef.h>
#include <string.h>

struct solar_os_display_link {
  u8g2_t u8g2;
  u8x8_display_info_t display_info;
  sdlp_host_t host;
  solar_os_display_link_config_t config;
  char bus[16];
  spi_device_handle_t spi;
  SemaphoreHandle_t mutex;
  uint8_t *mono, *row, *wire, *retained;
  size_t wire_size;
  bool retained_valid;
  uint16_t *pixels;
  uint32_t spi_hz, frame_id;
  uint64_t last_cs_us;
  esp_err_t last_error;
  bool ready_enabled, pins_initialized;
};
static const char *TAG = "spi-display";
static esp_err_t result(int r) {
  if (!r)
    return ESP_OK;
  if (r == SDLP_TIMEOUT || r == SDLP_DEADLINE)
    return ESP_ERR_TIMEOUT;
  if (r == SDLP_NO_MEMORY)
    return ESP_ERR_NO_MEM;
  if (r == SDLP_UNSUPPORTED)
    return ESP_ERR_NOT_SUPPORTED;
  if (r == SDLP_BAD_ARGUMENT)
    return ESP_ERR_INVALID_ARG;
  return ESP_FAIL;
}
static uint64_t now_us(void *ctx) {
  (void)ctx;
  return (uint64_t)esp_timer_get_time();
}
static void sleep_us(void *ctx, uint32_t us) {
  (void)ctx;
  uint64_t end = now_us(NULL) + us;
  uint64_t time;
  while ((time = now_us(NULL)) < end) {
    uint32_t remaining = (uint32_t)(end - time);
    TickType_t ticks = pdMS_TO_TICKS(remaining / 1000);
    if (ticks > 1)
      vTaskDelay(ticks - 1);
    else
      esp_rom_delay_us(remaining > 1000 ? 1000 : remaining);
  }
}
static int exchange(void *ctx, const uint8_t tx[256], uint8_t rx[256],
                    uint32_t hz, uint32_t gap, uint64_t deadline) {
  solar_os_display_link_t *d = ctx;
  uint64_t time = now_us(NULL);
  while (time < d->last_cs_us + gap ||
         (d->ready_enabled && !gpio_get_level(d->config.ready))) {
    if (time >= deadline)
      return -1;
    sleep_us(NULL, 100);
    time = now_us(NULL);
  }
  if (time >= deadline)
    return -1;
  if (!d->spi || d->spi_hz != hz) {
    if (d->spi) {
      if (spi_bus_remove_device(d->spi) != ESP_OK)
        return -1;
      d->spi = NULL;
    }
    spi_device_interface_config_t cfg = {.clock_speed_hz = (int)hz,
                                         .mode = 1,
                                         .spics_io_num = -1,
                                         .queue_size = 1};
    if (solar_os_bus_spi_add_device(d->bus, &cfg, &d->spi) != ESP_OK)
      return -1;
    d->spi_hz = hz;
  }
  time = now_us(NULL);
  if (time >= deadline)
    return -1;
  /* ESP-IDF only accepts portMAX_DELAY for bus acquisition. Recheck the
   * protocol deadline after obtaining the bus, before asserting CS. */
  if (spi_device_acquire_bus(d->spi, portMAX_DELAY) != ESP_OK)
    return -1;
  if (now_us(NULL) >= deadline) {
    spi_device_release_bus(d->spi);
    return -1;
  }
  esp_err_t err = gpio_set_level(d->config.cs, 0);
  if (err == ESP_OK) {
    /* Allow a software SPI-slave output-enable handler to observe CS. */
    esp_rom_delay_us(10);
    spi_transaction_t tr = {
        .length = 2048, .rxlength = 2048, .tx_buffer = tx, .rx_buffer = rx};
    err = spi_device_polling_transmit(d->spi, &tr);
    esp_rom_delay_us(2);
  }
  esp_err_t cs_err = gpio_set_level(d->config.cs, 1);
  d->last_cs_us = now_us(NULL);
  spi_device_release_bus(d->spi);
  return err == ESP_OK && cs_err == ESP_OK ? 0 : -1;
}
static esp_err_t connect_endpoint(solar_os_display_link_t *d, bool reset) {
  d->retained_valid = false;
  d->ready_enabled = false;
  if (reset && d->config.reset >= 0) {
    gpio_set_level(d->config.reset, d->config.reset_active_high ? 1 : 0);
    sleep_us(NULL, d->config.reset_ms * 1000);
    gpio_set_level(d->config.reset, d->config.reset_active_high ? 0 : 1);
  }
  sdlp_info_t old_info = d->host.info;
  int r = sdlp_host_discover(&d->host,
                             now_us(NULL) + (uint64_t)d->config.boot_ms * 1000);
  if (r)
    return result(r);
  if (d->host.info.width > 2040 || d->host.info.height > 2040 ||
      (size_t)d->host.info.width * d->host.info.height > 1024 * 1024 ||
      d->host.info.present_ms > 120000 ||
      (old_info.width && (old_info.width != d->host.info.width ||
                          old_info.height != d->host.info.height))) {
    d->host.info = old_info;
    return ESP_ERR_INVALID_SIZE;
  }
  if (d->config.ready >= 0 && !(d->host.info.capabilities & SDLP_READY))
    return ESP_ERR_NOT_SUPPORTED;
  unsigned format = d->host.info.preferred_format;
  uint32_t session;
  do {
    session = esp_random();
  } while (!session || session == d->host.session);
  r = sdlp_host_configure(&d->host, session, format, d->config.clock_hz);
  if (!r) r = sdlp_host_extensions(&d->host);
  if (!r)
    d->ready_enabled = d->config.ready >= 0;
  return result(r);
}
static void pack_row(solar_os_display_link_t *d, uint16_t y) {
  const unsigned format = d->host.format;
  uint16_t w = d->host.info.width;
  memset(d->row, 0, sdlp_row_bytes(w, format));
  for (uint16_t x = 0; x < w; x++) {
    uint16_t c = d->pixels[(size_t)y * w + x];
    if (format == SDLP_RGB565_BE) {
      d->row[2 * x] = (uint8_t)(c >> 8);
      d->row[2 * x + 1] = (uint8_t)c;
      continue;
    }
    unsigned red = (c >> 11) * 255 / 31, green = ((c >> 5) & 63) * 255 / 63,
             blue = (c & 31) * 255 / 31;
    unsigned gray = (red * 77 + green * 150 + blue * 29 + 128) >> 8;
    if (format == SDLP_GRAY8) d->row[x] = (uint8_t)gray;
    else if (format == SDLP_MONO1)
      d->row[x / 8] |= (uint8_t)((gray < 128 ? 1 : 0) << (x % 8));
    else {
      unsigned bits = format == SDLP_GRAY2 ? 2 : 4, max = (1u << bits) - 1;
      d->row[x * bits / 8] |=
          (uint8_t)(((gray * max + 127) / 255) << ((x * bits) % 8));
    }
  }
}
static esp_err_t upload(solar_os_display_link_t *d) {
  if (!d->host.configured) {
    esp_err_t e = connect_endpoint(d, false);
    if (e != ESP_OK && d->config.reset >= 0)
      e = connect_endpoint(d, true);
    if (e != ESP_OK)
      return e;
    d->frame_id = 0;
  }
  if (d->frame_id == UINT32_MAX) {
    d->host.configured = false;
    return ESP_ERR_INVALID_STATE;
  }
  uint32_t frame = ++d->frame_id;
  const unsigned format = d->host.format;
  uint16_t w = d->host.info.width, h = d->host.info.height;
  uint32_t row_bytes = sdlp_row_bytes(w, format);
  if (d->wire && d->wire_size >= (size_t)row_bytes * h) {
    for (uint16_t y = 0; y < h; y++) {
      pack_row(d, y);
      memcpy(d->wire + (size_t)y * row_bytes, d->row, row_bytes);
      if ((uint32_t)w * h > 65536 && (y & 31) == 31) vTaskDelay(1);
    }
    int r = sdlp_host_upload_frame(&d->host, frame, d->wire,
                                    (size_t)row_bytes * h, d->retained,
                                    &d->retained_valid);
    if (r) {
      d->retained_valid = false;
      d->host.configured = false;
      ESP_LOGE(TAG, "presentation failed: DLP %d", r);
    }
    return result(r);
  }
  d->retained_valid = false;
  int r = sdlp_host_begin_frame(&d->host, frame, true);
  uint32_t rows_per_rect = d->host.info.max_rect_bytes / row_bytes;
  if (rows_per_rect > h)
    rows_per_rect = h;
  uint32_t rectangle = 0, offset = 0, band_end = 0;
  uint8_t chunk[SDLP_DATA_SIZE];
  size_t used = 0;
  for (uint16_t y = 0; !r && y < h; y++) {
    if (y == band_end) {
      uint32_t rows = h - y;
      if (rows > rows_per_rect)
        rows = rows_per_rect;
      band_end = y + rows;
      rectangle++;
      offset = 0;
      used = 0;
      r = sdlp_host_begin_rect(&d->host, frame, rectangle, 0, y, w,
                               (uint16_t)rows);
      if (r)
        break;
    }
    pack_row(d, y);
    for (uint32_t pos = 0; !r && pos < row_bytes;) {
      size_t n = row_bytes - pos;
      if (n > sizeof(chunk) - used)
        n = sizeof(chunk) - used;
      memcpy(chunk + used, d->row + pos, n);
      used += n;
      pos += (uint32_t)n;
      if (used == sizeof(chunk)) {
        r = sdlp_host_write(&d->host, frame, rectangle, offset, chunk, used);
        offset += (uint32_t)used;
        used = 0;
      }
    }
    if (!r && (uint32_t)y + 1 == band_end && used) {
      r = sdlp_host_write(&d->host, frame, rectangle, offset, chunk, used);
      used = 0;
    }
    if ((uint32_t)w * h > 65536 && (y & 31) == 31)
      vTaskDelay(1);
  }
  if (!r)
    r = sdlp_host_present(&d->host, frame);
  if (!r)
    r = sdlp_host_wait_frame(
        &d->host, frame,
        now_us(NULL) + (uint64_t)d->host.info.present_ms * 1000 + 1000000);
  if (r) {
    d->host.configured = false;
    ESP_LOGE(TAG, "presentation failed: SDLP %d", r);
  }
  return result(r);
}
static solar_os_display_link_t *from_u8x8(u8x8_t *u) {
  return (solar_os_display_link_t *)((uint8_t *)u -
                                     offsetof(solar_os_display_link_t, u8g2) -
                                     offsetof(u8g2_t, u8x8));
}
static uint8_t display_cb(u8x8_t *u, uint8_t msg, uint8_t arg, void *ptr) {
  (void)ptr;
  solar_os_display_link_t *d = from_u8x8(u);
  if (msg == U8X8_MSG_DISPLAY_SETUP_MEMORY) {
    u8x8_d_helper_display_setup_memory(u, &d->display_info);
    return 1;
  }
  if (msg == U8X8_MSG_DISPLAY_DRAW_TILE)
    return 1;
  if (msg == U8X8_MSG_DISPLAY_REFRESH) {
    xSemaphoreTake(d->mutex, portMAX_DELAY);
    uint16_t w = d->host.info.width, h = d->host.info.height;
    size_t stride = (size_t)d->display_info.tile_width * 8;
    for (uint16_t y = 0; y < h; y++)
      for (uint16_t x = 0; x < w; x++)
        d->pixels[(size_t)y * w + x] =
            (d->mono[(size_t)(y / 8) * stride + x] & (1u << (y % 8))) ? 0
                                                                      : 0xffff;
    d->last_error = upload(d);
    xSemaphoreGive(d->mutex);
    return d->last_error == ESP_OK;
  }
  if (msg == U8X8_MSG_DISPLAY_SET_POWER_SAVE &&
      (d->host.info.capabilities & SDLP_POWER_SAVE)) {
    xSemaphoreTake(d->mutex, portMAX_DELAY);
    uint8_t p = arg ? 1 : 0;
    d->last_error =
        result(sdlp_host_command(&d->host, SDLP_SET_POWER, &p, 1, NULL, 0));
    xSemaphoreGive(d->mutex);
    return d->last_error == ESP_OK;
  }
  return 1;
}
esp_err_t solar_os_display_link_create(const solar_os_display_link_config_t *c,
                                       solar_os_display_link_t **out) {
  if (!c || !out || !c->bus || !GPIO_IS_VALID_OUTPUT_GPIO(c->cs) ||
      (c->ready >= 0 && !GPIO_IS_VALID_GPIO(c->ready)) ||
      (c->reset >= 0 && !GPIO_IS_VALID_OUTPUT_GPIO(c->reset)) ||
      c->cs == c->ready || c->cs == c->reset ||
      (c->ready >= 0 && c->ready == c->reset))
    return ESP_ERR_INVALID_ARG;
  *out = NULL;
  solar_os_display_link_t *d = solar_os_memory_calloc(
      1, sizeof(*d), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "display.link");
  if (!d)
    return ESP_ERR_NO_MEM;
  d->config = *c;
  strlcpy(d->bus, c->bus, sizeof(d->bus));
  d->config.bus = d->bus;
  d->mutex = xSemaphoreCreateMutex();
  esp_err_t e = ESP_ERR_NO_MEM;
  if (!d->mutex)
    goto fail;
  gpio_set_level(c->cs, 1);
  gpio_config_t pins = {.pin_bit_mask = UINT64_C(1) << c->cs,
                        .mode = GPIO_MODE_OUTPUT};
  if (c->reset >= 0) {
    gpio_set_level(c->reset, c->reset_active_high ? 0 : 1);
    pins.pin_bit_mask |= UINT64_C(1) << c->reset;
  }
  e = gpio_config(&pins);
  if (e != ESP_OK)
    goto fail;
  d->pins_initialized = true;
  if (c->ready >= 0) {
    pins.pin_bit_mask = UINT64_C(1) << c->ready;
    pins.mode = GPIO_MODE_INPUT;
    pins.pull_down_en = GPIO_PULLDOWN_ENABLE;
    e = gpio_config(&pins);
    if (e != ESP_OK)
      goto fail;
  }
  sdlp_transport_t t = {
      .ctx = d, .now_us = now_us, .sleep_us = sleep_us, .exchange = exchange};
  sdlp_host_init(&d->host, &t);
  d->host.discovery_sequence = esp_random() & 0x7fffffff;
  e = connect_endpoint(d, false);
  if (e != ESP_OK && c->reset >= 0)
    e = connect_endpoint(d, true);
  if (e != ESP_OK)
    goto fail;
  uint16_t w = d->host.info.width, h = d->host.info.height;
  d->display_info = (u8x8_display_info_t){.tile_width = (uint8_t)((w + 7) / 8),
                                          .tile_height = (uint8_t)((h + 7) / 8),
                                          .pixel_width = w,
                                          .pixel_height = h};
  d->mono = solar_os_memory_calloc(
      (size_t)d->display_info.tile_width * d->display_info.tile_height, 8,
      SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "display.link.mono");
  d->pixels = solar_os_memory_alloc((size_t)w * h * 2,
                                    SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                    "display.link.rgb");
  d->row = solar_os_memory_alloc(
      (size_t)w * 2, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "display.link.row");
  /* Delta caches are optional and bounded. Large displays or memory pressure
   * retain the streaming full-frame path rather than failing attachment. */
  d->wire_size = (size_t)sdlp_row_bytes(w, d->host.format) * h;
  if (d->wire_size <= 256u * 1024u) {
    d->wire = solar_os_memory_alloc(d->wire_size, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                     "display.link.wire");
    d->retained = solar_os_memory_alloc(d->wire_size, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                         "display.link.retained");
    if (!d->wire || !d->retained) {
      solar_os_memory_free(d->wire); solar_os_memory_free(d->retained);
      d->wire = d->retained = NULL;
    }
  }
  if (!d->mono || !d->pixels || !d->row) {
    e = ESP_ERR_NO_MEM;
    goto fail;
  }
  memset(d->pixels, 0xff, (size_t)w * h * 2);
  u8g2_SetupDisplay(&d->u8g2, display_cb, u8x8_cad_empty, u8x8_dummy_cb,
                    u8x8_dummy_cb);
  u8g2_SetupBuffer(&d->u8g2, d->mono, d->display_info.tile_height,
                   u8g2_ll_hvline_vertical_top_lsb, U8G2_R0);
  u8g2_InitDisplay(&d->u8g2);
  u8g2_ClearBuffer(&d->u8g2);
  *out = d;
  return ESP_OK;
fail:
  solar_os_display_link_destroy(d);
  return e;
}
void solar_os_display_link_destroy(solar_os_display_link_t *d) {
  if (!d)
    return;
  if (d->spi)
    spi_bus_remove_device(d->spi);
  if (d->pins_initialized) {
    gpio_set_level(d->config.cs, 1);
    gpio_reset_pin(d->config.cs);
    if (d->config.ready >= 0)
      gpio_reset_pin(d->config.ready);
    if (d->config.reset >= 0) {
      gpio_set_level(d->config.reset, d->config.reset_active_high ? 0 : 1);
      gpio_reset_pin(d->config.reset);
    }
  }
  if (d->mutex)
    vSemaphoreDelete(d->mutex);
  solar_os_memory_free(d->mono);
  solar_os_memory_free(d->pixels);
  solar_os_memory_free(d->row);
  solar_os_memory_free(d->wire);
  solar_os_memory_free(d->retained);
  solar_os_memory_free(d);
}
u8g2_t *solar_os_display_link_u8g2(solar_os_display_link_t *d) {
  return d ? &d->u8g2 : NULL;
}
const sdlp_info_t *
solar_os_display_link_info(const solar_os_display_link_t *d) {
  return d ? &d->host.info : NULL;
}

static bool raster_valid(const uint8_t *data, size_t size, uint16_t w,
                         uint16_t h, uint16_t stride,
                         solar_os_display_format_t format,
                         const uint16_t *palette, size_t palette_size) {
  unsigned bits;
  switch (format) {
  case SOLAR_OS_DISPLAY_FORMAT_MONO1:
    bits = 1;
    break;
  case SOLAR_OS_DISPLAY_FORMAT_INDEX2:
    bits = 2;
    break;
  case SOLAR_OS_DISPLAY_FORMAT_INDEX8:
    bits = 8;
    break;
  case SOLAR_OS_DISPLAY_FORMAT_RGB565:
    bits = 16;
    break;
  default:
    return false;
  }
  size_t row = ((size_t)w * bits + 7) / 8;
  if (!data || !w || !h || stride < row ||
      size < (size_t)(h - 1) * stride + row)
    return false;
  if (format == SOLAR_OS_DISPLAY_FORMAT_INDEX2 &&
      (!palette || palette_size < 4))
    return false;
  if (format == SOLAR_OS_DISPLAY_FORMAT_INDEX8 &&
      (!palette || palette_size < 256))
    return false;
  if (format == SOLAR_OS_DISPLAY_FORMAT_MONO1 && palette && palette_size < 2)
    return false;
  return true;
}
static uint16_t color(const uint8_t *data, uint16_t stride, unsigned x,
                      unsigned y, solar_os_display_format_t format,
                      const uint16_t *palette, bool invert) {
  const uint8_t *p = data + (size_t)y * stride;
  unsigned index;
  if (format == SOLAR_OS_DISPLAY_FORMAT_RGB565)
    return (uint16_t)((uint16_t)p[2 * x] << 8 | p[2 * x + 1]);
  if (format == SOLAR_OS_DISPLAY_FORMAT_INDEX8)
    return palette[p[x]];
  if (format == SOLAR_OS_DISPLAY_FORMAT_INDEX2) {
    index = (p[x / 4] >> ((x % 4) * 2)) & 3;
    return palette[index];
  }
  index = (p[x / 8] >> (x % 8)) & 1;
  if (invert)
    index ^= 1;
  return palette ? palette[index] : (index ? 0 : 0xffff);
}
static void native_to_logical(unsigned rot, uint16_t w, uint16_t h, unsigned nx,
                              unsigned ny, unsigned *x, unsigned *y) {
  switch (rot) {
  case 1:
    *x = ny;
    *y = w - 1 - nx;
    break;
  case 2:
    *x = w - 1 - nx;
    *y = h - 1 - ny;
    break;
  case 3:
    *x = h - 1 - ny;
    *y = nx;
    break;
  default:
    *x = nx;
    *y = ny;
    break;
  }
}
esp_err_t solar_os_display_link_surface(void *ctx,
                                        const solar_os_display_surface_t *s) {
  solar_os_display_link_t *d = ctx;
  if (!d || !s)
    return ESP_ERR_INVALID_ARG;
  uint16_t w = d->host.info.width, h = d->host.info.height;
  bool rotated = s->rotation == 1 || s->rotation == 3;
  if (s->rotation > 3 || s->native_width != w || s->native_height != h ||
      s->width != (rotated ? h : w) || s->height != (rotated ? w : h) ||
      !raster_valid(s->data, s->data_size, s->width, s->height, s->stride,
                    s->format, s->palette_rgb565, s->palette_size))
    return ESP_ERR_INVALID_ARG;
  xSemaphoreTake(d->mutex, portMAX_DELAY);
  for (unsigned ny = 0; ny < h; ny++)
    for (unsigned nx = 0; nx < w; nx++) {
      unsigned x, y;
      native_to_logical(s->rotation, w, h, nx, ny, &x, &y);
      d->pixels[(size_t)ny * w + nx] =
          color(s->data, s->stride, x, y, s->format, s->palette_rgb565, false);
    }
  esp_err_t e = upload(d);
  d->last_error = e;
  xSemaphoreGive(d->mutex);
  return e;
}
esp_err_t solar_os_display_link_frame(void *ctx,
                                      const solar_os_display_raster_t *f) {
  solar_os_display_link_t *d = ctx;
  if (!d || !f)
    return ESP_ERR_INVALID_ARG;
  unsigned rot = d->u8g2.cb == U8G2_R1   ? 1
                 : d->u8g2.cb == U8G2_R2 ? 2
                 : d->u8g2.cb == U8G2_R3 ? 3
                                         : 0;
  uint16_t w = d->host.info.width, h = d->host.info.height,
           lw = (rot & 1) ? h : w, lh = (rot & 1) ? w : h;
  if (!f->width || !f->height || (uint32_t)f->x + f->width > lw ||
      (uint32_t)f->y + f->height > lh ||
      !raster_valid(f->data, f->data_size, f->source_width, f->source_height,
                    f->source_stride, f->format, f->palette_rgb565,
                    f->palette_size))
    return ESP_ERR_INVALID_ARG;
  uint16_t bg = f->background_rgb565;
  if (f->format != SOLAR_OS_DISPLAY_FORMAT_RGB565) {
    if (f->palette_rgb565) {
      if (f->background_index >= f->palette_size)
        return ESP_ERR_INVALID_ARG;
      bg = f->palette_rgb565[f->background_index];
    } else
      bg = ((f->background_index != 0) ^ f->palette_inverted) ? 0 : 0xffff;
  }
  xSemaphoreTake(d->mutex, portMAX_DELAY);
  for (unsigned ny = 0; ny < h; ny++)
    for (unsigned nx = 0; nx < w; nx++) {
      unsigned x, y;
      native_to_logical(rot, w, h, nx, ny, &x, &y);
      if (x >= f->x && y >= f->y && x < (unsigned)f->x + f->width &&
          y < (unsigned)f->y + f->height) {
        unsigned sx = (x - f->x) * (uint32_t)f->source_width / f->width,
                 sy = (y - f->y) * (uint32_t)f->source_height / f->height;
        d->pixels[(size_t)ny * w + nx] =
            color(f->data, f->source_stride, sx, sy, f->format,
                  f->palette_rgb565, f->palette_inverted);
      } else if (f->clear_background)
        d->pixels[(size_t)ny * w + nx] = bg;
    }
  esp_err_t e = upload(d);
  d->last_error = e;
  xSemaphoreGive(d->mutex);
  return e;
}
esp_err_t solar_os_display_link_power_state(void *ctx) {
  solar_os_display_link_t *d = ctx;
  if (!d)
    return ESP_ERR_INVALID_ARG;
  u8g2_SendBuffer(&d->u8g2);
  return d->last_error;
}
