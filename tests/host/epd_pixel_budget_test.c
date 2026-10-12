#include "epd_pixel_budget.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    uint8_t pixels[4];
    epd_pixel_budget_t b;
    assert(epd_pixel_budget_init(&b, pixels, 4) == ESP_OK);
    for (size_t i = 0; i < 4; ++i)
        epd_pixel_budget_seed(&b, i, true);
    epd_pixel_budget_target(&b, 0, false, 3);
    epd_pixel_budget_target(&b, 1, false, 1);
    assert(b.pending == 2);
    assert(epd_pixel_budget_drive(&b, 2) == EPD_PIXEL_HOLD);
    epd_pixel_budget_advance(&b);
    assert(b.pending == 1 && epd_pixel_budget_drive(&b, 1) == EPD_PIXEL_HOLD);
    epd_pixel_budget_target(&b, 0, false, 3); /* Must not refill 2 remaining. */
    epd_pixel_budget_advance(&b);
    epd_pixel_budget_advance(&b);
    assert(b.pending == 0);
    epd_pixel_budget_target(&b, 0, true, 3);
    epd_pixel_budget_advance(&b);
    epd_pixel_budget_target(&b, 0, false, 2); /* Mid-transition reversal. */
    assert(epd_pixel_budget_drive(&b, 0) == EPD_PIXEL_DRIVE_BLACK);
    epd_pixel_budget_advance(&b);
    assert(b.pending == 1);
    epd_pixel_budget_seed(&b, 0, true); /* Known full-cleanup state. */
    assert(b.pending == 0);
    epd_pixel_budget_target(&b, 0, false, 32);
    for (unsigned i = 0; i < 32; ++i)
        epd_pixel_budget_advance(&b);
    assert(b.pending == 0 && epd_pixel_budget_drive(&b, 0) == EPD_PIXEL_HOLD);
    epd_pixel_budget_target(&b, 0, true, 0);
    epd_pixel_budget_target(&b, 4, true, 1);
    assert(b.pending == 0);

    epd_pulse_tuning_t t = epd_pulse_default_tuning();
    assert(epd_pulse_tuning_set(&t, "drive-cycles=6") == ESP_OK);
    assert(t.drive_cycles == 6);
    assert(epd_pulse_tuning_set(&t, "pulse-frames=8") == ESP_OK);
    assert(epd_pulse_tuning_set(&t, "pulse-frames=16") == ESP_OK && t.pulse_frames == 16);
    assert(epd_pulse_tuning_set(&t, "pulse-frames=32") == ESP_OK && t.pulse_frames == 32);
    assert(epd_pulse_tuning_set(&t, "waveform-fr=4") == ESP_OK);
    assert(epd_pulse_tuning_set(&t, "cleanup-every=128") == ESP_OK);
    const epd_pulse_tuning_t before = t;
    const char *bad[] = {"drive-cycles=0", "drive-cycles=33", "drive-cycles=-1",
        "drive-cycles=+1", "drive-cycles=1x", "drive-cycles=99999999999999",
        "drive-cycles=", "pulse-frames=0", "pulse-frames=33", "waveform-fr=13", "cleanup-every=129"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        assert(epd_pulse_tuning_set(&t, bad[i]) == ESP_ERR_INVALID_ARG);
        assert(memcmp(&t, &before, sizeof(t)) == 0);
    }
    assert(epd_pulse_tuning_set(&t, "refresh=pulse") == ESP_ERR_NOT_FOUND);
    puts("EPD pixel budgeting tests: ok");
}
