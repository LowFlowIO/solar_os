#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Controller-independent monochrome pulse accounting. A cycle means one
 * successfully completed panel waveform, not an emulator/rendered frame.
 * The caller owns storage, pixel order, transport, and waveform calibration.
 */
typedef enum {
    EPD_PIXEL_HOLD,
    EPD_PIXEL_DRIVE_BLACK,
    EPD_PIXEL_DRIVE_WHITE,
} epd_pixel_drive_t;

typedef struct {
    uint8_t drive_cycles;   /* 1..32 successful waveforms per changed pixel */
    uint8_t pulse_frames;   /* 1..32 panel scan frames per drive phase */
    uint8_t waveform_fr;    /* 1..12 raw controller frame-rate code */
    uint8_t cleanup_every;  /* 1..128 completed image updates */
} epd_pulse_tuning_t;

typedef struct {
    uint8_t *pixels; /* One byte/pixel: target color + remaining cycles. */
    size_t count;
    size_t pending;
} epd_pixel_budget_t;

epd_pulse_tuning_t epd_pulse_default_tuning(void);
/* Shared display-mode keys. Unknown keys return NOT_FOUND; bad values return
 * INVALID_ARG without changing tuning. waveform_fr is a code, not Hz.
 */
esp_err_t epd_pulse_tuning_set(epd_pulse_tuning_t *tuning, const char *mode);
esp_err_t epd_pixel_budget_init(epd_pixel_budget_t *budget, uint8_t *storage,
                                size_t pixels);
void epd_pixel_budget_seed(epd_pixel_budget_t *budget, size_t pixel, bool white);
void epd_pixel_budget_target(epd_pixel_budget_t *budget, size_t pixel, bool white,
                              uint8_t cycles);
epd_pixel_drive_t epd_pixel_budget_drive(const epd_pixel_budget_t *budget,
                                        size_t pixel);
/* Commit only after the complete waveform succeeds. Transport failures must
 * not consume budget or mark the optical state as settled.
 */
void epd_pixel_budget_advance(epd_pixel_budget_t *budget);
