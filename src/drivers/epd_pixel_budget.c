#include "epd_pixel_budget.h"

#include <string.h>

#define WHITE_BIT 0x80U
#define CYCLES_MASK 0x3fU
#define LISTED_BIT 0x40U

epd_pulse_tuning_t epd_pulse_default_tuning(void)
{
    return (epd_pulse_tuning_t){1, 1, 8, 32};
}

esp_err_t epd_pulse_tuning_set(epd_pulse_tuning_t *t, const char *mode)
{
    if (t == NULL || mode == NULL)
        return ESP_ERR_INVALID_ARG;
    struct option { const char *key; uint8_t *value; unsigned max; } options[] = {
        {"drive-cycles=", &t->drive_cycles, 32},
        {"pulse-frames=", &t->pulse_frames, 32},
        {"waveform-fr=", &t->waveform_fr, 12},
        {"cleanup-every=", &t->cleanup_every, 128},
    };
    for (size_t i = 0; i < sizeof(options) / sizeof(options[0]); ++i) {
        const size_t n = strlen(options[i].key);
        if (strncmp(mode, options[i].key, n) != 0)
            continue;
        const char *p = mode + n;
        unsigned value = 0;
        if (*p == '\0')
            return ESP_ERR_INVALID_ARG;
        for (; *p != '\0'; ++p) {
            if (*p < '0' || *p > '9' || value > options[i].max)
                return ESP_ERR_INVALID_ARG;
            value = value * 10U + (unsigned)(*p - '0');
        }
        if (value == 0 || value > options[i].max)
            return ESP_ERR_INVALID_ARG;
        *options[i].value = (uint8_t)value;
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t epd_pixel_budget_init(epd_pixel_budget_t *b, uint8_t *storage,
                                size_t pixels)
{
    if (b == NULL || storage == NULL || pixels == 0)
        return ESP_ERR_INVALID_ARG;
    *b = (epd_pixel_budget_t){.pixels = storage, .count = pixels};
    memset(storage, 0, pixels);
    return ESP_OK;
}

esp_err_t epd_pixel_budget_set_active_storage(epd_pixel_budget_t *b,
                                             uint32_t *storage, size_t entries)
{
    if (b == NULL || b->pixels == NULL || storage == NULL || entries < b->count ||
        b->count > UINT32_MAX || b->pending != 0 || b->active_count != 0)
        return ESP_ERR_INVALID_ARG;
    b->active = storage;
    return ESP_OK;
}

void epd_pixel_budget_reset(epd_pixel_budget_t *b)
{
    if (b == NULL || b->pixels == NULL)
        return;
    memset(b->pixels, 0, b->count);
    b->pending = 0;
    b->active_count = 0;
}

void epd_pixel_budget_seed(epd_pixel_budget_t *b, size_t p, bool white)
{
    if (b == NULL || p >= b->count)
        return;
    if ((b->pixels[p] & CYCLES_MASK) != 0)
        --b->pending;
    b->pixels[p] = (white ? WHITE_BIT : 0) | (b->pixels[p] & LISTED_BIT);
}

void epd_pixel_budget_target(epd_pixel_budget_t *b, size_t p, bool white,
                              uint8_t cycles)
{
    if (b == NULL || p >= b->count || cycles == 0 || cycles > 32)
        return;
    const uint8_t color = white ? WHITE_BIT : 0;
    if ((b->pixels[p] & WHITE_BIT) == color)
        return; /* Same target retains its remaining budget, never refills it. */
    if ((b->pixels[p] & CYCLES_MASK) == 0)
        ++b->pending;
    uint8_t listed = b->pixels[p] & LISTED_BIT;
    if (b->active != NULL && listed == 0) {
        b->active[b->active_count++] = (uint32_t)p;
        listed = LISTED_BIT;
    }
    b->pixels[p] = color | listed | cycles;
}

epd_pixel_drive_t epd_pixel_budget_drive(const epd_pixel_budget_t *b, size_t p)
{
    if (b == NULL || p >= b->count ||
        (b->pixels[p] & CYCLES_MASK) == 0)
        return EPD_PIXEL_HOLD;
    return b->pixels[p] & WHITE_BIT ? EPD_PIXEL_DRIVE_WHITE :
                                    EPD_PIXEL_DRIVE_BLACK;
}

void epd_pixel_budget_advance(epd_pixel_budget_t *b)
{
    if (b == NULL)
        return;
    if (b->active != NULL) {
        size_t keep = 0;
        for (size_t i = 0; i < b->active_count; ++i) {
            const size_t p = b->active[i];
            if ((b->pixels[p] & CYCLES_MASK) != 0) {
                --b->pixels[p];
                if ((b->pixels[p] & CYCLES_MASK) == 0)
                    --b->pending;
            }
            if ((b->pixels[p] & CYCLES_MASK) != 0)
                b->active[keep++] = (uint32_t)p;
            else
                b->pixels[p] &= (uint8_t)~LISTED_BIT;
        }
        b->active_count = keep;
        return;
    }
    for (size_t p = 0; p < b->count; ++p) {
        if ((b->pixels[p] & CYCLES_MASK) != 0) {
            --b->pixels[p];
            if ((b->pixels[p] & CYCLES_MASK) == 0)
                --b->pending;
        }
    }
}
