/* SPDX-License-Identifier: MIT */
#ifndef SDLP_H
#define SDLP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define SDLP_CELL_SIZE 256u
#define SDLP_PAYLOAD_SIZE 220u
#define SDLP_DATA_SIZE 208u
#define SDLP_FORMAT_BIT(n) (UINT32_C(1) << (n))
#define SDLP_FORMATS UINT32_C(31)
enum { SDLP_IDLE, SDLP_REQUEST, SDLP_RESPONSE };
enum { SDLP_MONO1, SDLP_GRAY2, SDLP_GRAY4, SDLP_GRAY8, SDLP_RGB565_BE };
enum {
  SDLP_OK,
  SDLP_UNSUPPORTED,
  SDLP_BAD_ARGUMENT,
  SDLP_BAD_STATE,
  SDLP_BUSY,
  SDLP_BAD_SESSION,
  SDLP_STALE_BOOT,
  SDLP_BAD_SEQUENCE,
  SDLP_NO_MEMORY,
  SDLP_IO_ERROR,
  SDLP_TIMEOUT
};
enum {
  SDLP_GET_INFO = 1,
  SDLP_CONFIGURE = 2,
  SDLP_GET_STATUS = 3,
  SDLP_BEGIN_FRAME = 0x10,
  SDLP_BEGIN_RECT,
  SDLP_RECT_DATA,
  SDLP_PRESENT,
  SDLP_ABORT_FRAME,
  SDLP_SET_BRIGHTNESS = 0x20,
  SDLP_SET_POWER
};
enum {
  SDLP_PRESERVE_CONTENT = 1,
  SDLP_STAGED_FRAME = 2,
  SDLP_BRIGHTNESS = 4,
  SDLP_POWER_SAVE = 8,
  SDLP_READY = 16
};
enum {
  SDLP_STATE_IDLE,
  SDLP_STATE_FRAME_OPEN,
  SDLP_STATE_PRESENTING,
  SDLP_STATE_POWER_SAVE,
  SDLP_STATE_FAULT
};
enum { SDLP_FULL_REPLACE = 1 };
enum { SDLP_LINK_ERROR = -1, SDLP_DEADLINE = -2, SDLP_INVALID_CELL = -3 };
typedef struct {
  uint8_t kind, opcode;
  uint32_t session, sequence;
  uint16_t result, length;
  uint64_t boot;
  uint8_t payload[SDLP_PAYLOAD_SIZE];
} sdlp_cell_t;
typedef struct {
  uint16_t width, height, preferred_format;
  uint32_t formats, capabilities, max_clock_hz, min_gap_us, max_rect_bytes;
  uint32_t command_ms, present_ms, nominal_refresh_ms;
  uint8_t identity[16];
  char model[32], firmware[32];
} sdlp_info_t;
typedef struct {
  uint8_t state, brightness;
  uint16_t error;
  uint32_t active_frame, completed_frame, failed_frame, rectangle, offset;
} sdlp_status_t;
uint16_t sdlp_get_u16(const uint8_t *p);
uint32_t sdlp_get_u32(const uint8_t *p);
void sdlp_put_u16(uint8_t *p, uint16_t v);
void sdlp_put_u32(uint8_t *p, uint32_t v);
uint32_t sdlp_crc32(const void *data, size_t size);
bool sdlp_encode(uint8_t out[SDLP_CELL_SIZE], const sdlp_cell_t *cell);
bool sdlp_decode(sdlp_cell_t *cell, const uint8_t in[SDLP_CELL_SIZE]);
bool sdlp_info_valid(const sdlp_info_t *info);
void sdlp_info_encode(uint8_t out[128], const sdlp_info_t *info);
bool sdlp_info_decode(sdlp_info_t *info, const uint8_t *data, size_t size);
bool sdlp_status_decode(sdlp_status_t *status, const uint8_t *data,
                        size_t size);
uint32_t sdlp_row_bytes(uint16_t width, unsigned format);

/* Callbacks run in the caller's context. Return wire results. present starts
 * asynchronously; poll_present returns BUSY until completed, OK or an error.
 * All other callbacks must finish within info.command_ms. No allocations. */
typedef struct {
  void *ctx;
  int (*configure)(void *, unsigned format);
  int (*begin_frame)(void *, uint32_t frame, bool full);
  int (*begin_rect)(void *, uint16_t x, uint16_t y, uint16_t w, uint16_t h);
  int (*write)(void *, uint32_t offset, const uint8_t *data, size_t size);
  int (*present)(void *);
  int (*poll_present)(void *);
  int (*abort)(void *);
  int (*brightness)(void *, uint8_t percent);
  int (*power)(void *, bool save);
} sdlp_backend_t;
typedef struct {
  bool valid;
  uint8_t request[SDLP_CELL_SIZE], response[SDLP_CELL_SIZE];
} sdlp_replay_t;
typedef struct {
  sdlp_info_t info;
  sdlp_backend_t backend;
  uint64_t boot, present_started_us;
  uint32_t session, sequence, frame_high, rect_high, rect_total, row_bytes;
  uint16_t format, next_y, rect_height, row_used_bits;
  uint32_t frame_flags;
  bool need_full, have_rect;
  sdlp_status_t status;
  sdlp_replay_t replay, auxiliary;
} sdlp_device_t;
bool sdlp_device_init(sdlp_device_t *, const sdlp_info_t *, uint64_t boot,
                      const sdlp_backend_t *);
/* false means malformed or IDLE: retain currently queued TX. */
bool sdlp_device_receive(sdlp_device_t *, const uint8_t request[SDLP_CELL_SIZE],
                         uint8_t response[SDLP_CELL_SIZE], uint64_t now_us);
void sdlp_device_idle(const sdlp_device_t *, uint8_t out[SDLP_CELL_SIZE]);
void sdlp_device_tick(sdlp_device_t *, uint64_t now_us);

typedef struct {
  void *ctx;
  uint64_t (*now_us)(void *);
  void (*sleep_us)(void *, uint32_t);
  /* Respect gap and absolute deadline; optional READY is adapter-owned.
   * Release the shared bus after every exchange. Nonzero means I/O failure. */
  int (*exchange)(void *, const uint8_t tx[SDLP_CELL_SIZE],
                  uint8_t rx[SDLP_CELL_SIZE], uint32_t hz, uint32_t gap_us,
                  uint64_t deadline_us);
} sdlp_transport_t;
typedef struct {
  sdlp_transport_t transport;
  sdlp_info_t info;
  uint64_t boot;
  uint32_t session, sequence, discovery_sequence, clock_hz, gap_us;
  uint32_t command_limit_ms;
  unsigned format;
  bool configured;
} sdlp_host_t;
void sdlp_host_init(sdlp_host_t *, const sdlp_transport_t *);
int sdlp_host_discover(sdlp_host_t *, uint64_t deadline_us);
int sdlp_host_configure(sdlp_host_t *, uint32_t session, unsigned format,
                        uint32_t max_clock_hz);
/* deadline zero uses the descriptor command bound plus polling allowance. */
int sdlp_host_command(sdlp_host_t *, unsigned opcode, const void *payload,
                      size_t length, sdlp_cell_t *response,
                      uint64_t deadline_us);
int sdlp_host_begin_frame(sdlp_host_t *, uint32_t frame, bool full);
int sdlp_host_begin_rect(sdlp_host_t *, uint32_t frame, uint32_t rect,
                         uint16_t x, uint16_t y, uint16_t w, uint16_t h);
int sdlp_host_write(sdlp_host_t *, uint32_t frame, uint32_t rect,
                    uint32_t offset, const uint8_t *data, size_t size);
int sdlp_host_present(sdlp_host_t *, uint32_t frame);
int sdlp_host_wait_frame(sdlp_host_t *, uint32_t frame, uint64_t deadline_us);
#ifdef __cplusplus
}
#endif
#endif
