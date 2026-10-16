#include "sdlp.h"
#include "solar_os_display_surface.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE -2
#define ESP_LOGE(...)                                                          \
  do {                                                                         \
  } while (0)
typedef struct {
  sdlp_host_t host;
  struct {
    int reset;
  } config;
  uint16_t *pixels;
  uint8_t *row;
  uint32_t frame_id;
} solar_os_display_link_t;
typedef struct {
  sdlp_device_t device;
  uint8_t queued[256], *image;
  uint64_t now;
  uint32_t row_bytes, base, rectangles, writes;
} fixture_t;
static fixture_t fixture;
static uint64_t now_us(void *ctx) {
  (void)ctx;
  return fixture.now;
}
static void sleep_us(void *ctx, uint32_t us) {
  (void)ctx;
  fixture.now += us;
}
static void vTaskDelay(unsigned ticks) { fixture.now += ticks * 1000; }
static esp_err_t connect_endpoint(solar_os_display_link_t *d, bool reset) {
  (void)d;
  (void)reset;
  return ESP_FAIL;
}
static esp_err_t result(int r) { return r ? ESP_FAIL : ESP_OK; }
static int configure(void *ctx, unsigned f) {
  (void)ctx;
  (void)f;
  return 0;
}
static int begin_frame(void *ctx, uint32_t frame, bool full) {
  (void)ctx;
  (void)frame;
  assert(full);
  return 0;
}
static int begin_rect(void *ctx, uint16_t x, uint16_t y, uint16_t w,
                      uint16_t h) {
  fixture_t *f = ctx;
  (void)h;
  assert(x == 0 && w == f->device.info.width);
  f->base = y * f->row_bytes;
  f->rectangles++;
  return 0;
}
static int write_data(void *ctx, uint32_t offset, const uint8_t *p, size_t n) {
  fixture_t *f = ctx;
  assert(n <= 208);
  assert(f->base + offset + n <= f->row_bytes * f->device.info.height);
  memcpy(f->image + f->base + offset, p, n);
  f->writes++;
  return 0;
}
static int operation(void *ctx) {
  (void)ctx;
  return 0;
}
static int exchange(void *ctx, const uint8_t *tx, uint8_t *rx, uint32_t hz,
                    uint32_t gap, uint64_t deadline) {
  fixture_t *f = ctx;
  (void)deadline;
  f->now += gap + UINT64_C(2048000000) / hz;
  memcpy(rx, f->queued, 256);
  (void)sdlp_device_receive(&f->device, tx, f->queued, f->now);
  return 0;
}
#include "display_link_upload.inc"
static void run(unsigned format, uint16_t width, uint16_t height,
                unsigned rows) {
  fixture_t *f = &fixture;
  memset(f, 0, sizeof(*f));
  f->row_bytes = sdlp_row_bytes(width, format);
  f->image = calloc(height, f->row_bytes);
  assert(f->image);
  sdlp_info_t info = {.width = width,
                      .height = height,
                      .formats = SDLP_FORMAT_BIT(format),
                      .preferred_format = format,
                      .max_clock_hz = 1000000,
                      .min_gap_us = 5000,
                      .max_rect_bytes = f->row_bytes * rows,
                      .command_ms = 1000,
                      .present_ms = 1000};
  sdlp_backend_t b = {.ctx = f,
                      .configure = configure,
                      .begin_frame = begin_frame,
                      .begin_rect = begin_rect,
                      .write = write_data,
                      .present = operation,
                      .poll_present = operation,
                      .abort = operation};
  assert(sdlp_device_init(&f->device, &info, 42, &b));
  sdlp_device_idle(&f->device, f->queued);
  sdlp_transport_t t = {
      .ctx = f, .exchange = exchange, .now_us = now_us, .sleep_us = sleep_us};
  solar_os_display_link_t d = {.config = {.reset = -1}};
  sdlp_host_init(&d.host, &t);
  assert(!sdlp_host_discover(&d.host, 5000000));
  assert(!sdlp_host_configure(&d.host, 123, format, 1000000));
  d.pixels = malloc((size_t)width * height * 2);
  d.row = malloc(width * 2);
  assert(d.pixels && d.row);
  for (unsigned y = 0; y < height; y++)
    for (unsigned x = 0; x < width; x++)
      d.pixels[y * width + x] = (x + y) % 2 ? 0xffff : 0;
  assert(!upload(&d));
  assert(f->device.status.completed_frame == 1);
  for (unsigned y = 0; y < height; y++)
    for (unsigned x = 0; x < width; x++) {
      unsigned white = (x + y) % 2;
      const uint8_t *row = f->image + y * f->row_bytes;
      if (format == SDLP_MONO1)
        assert(((row[x / 8] >> (x % 8)) & 1) == !white);
      else if (format == SDLP_RGB565_BE)
        assert(row[x * 2] == (white ? 255 : 0) &&
               row[x * 2 + 1] == (white ? 255 : 0));
      else {
        unsigned bits = format == SDLP_GRAY2   ? 2
                        : format == SDLP_GRAY4 ? 4
                                               : 8,
                 max = (1u << bits) - 1;
        assert(((row[x * bits / 8] >> ((x * bits) % 8)) & max) ==
               (white ? max : 0));
      }
    }
  assert(f->rectangles == (height + rows - 1) / rows);
  if (width == 640 && height == 200)
    assert(f->writes == 77);
  free(d.pixels);
  free(d.row);
  free(f->image);
}
int main(void) {
  for (unsigned f = 0; f < 5; f++) {
    run(f, 13, 3, 1);
    run(f, 13, 3, 2);
  }
  run(SDLP_MONO1, 640, 200, 200);
  uint8_t mono[] = {0x15}, rgb[] = {0xf8, 0, 0, 0x1f};
  assert(
      raster_valid(mono, 1, 5, 1, 1, SOLAR_OS_DISPLAY_FORMAT_MONO1, NULL, 0));
  assert(
      !raster_valid(mono, 1, 8, 2, 1, SOLAR_OS_DISPLAY_FORMAT_MONO1, NULL, 0));
  assert(color(mono, 1, 0, 0, SOLAR_OS_DISPLAY_FORMAT_MONO1, NULL, false) == 0);
  assert(color(mono, 1, 0, 0, SOLAR_OS_DISPLAY_FORMAT_MONO1, NULL, true) ==
         0xffff);
  assert(color(rgb, 4, 1, 0, SOLAR_OS_DISPLAY_FORMAT_RGB565, NULL, false) ==
         0x001f);
  unsigned x, y;
  native_to_logical(1, 13, 3, 2, 1, &x, &y);
  assert(x == 1 && y == 10);
  puts("SolarOS upload bands, all wire formats, odd rows, palette polarity and "
       "rotation passed");
  return 0;
}
