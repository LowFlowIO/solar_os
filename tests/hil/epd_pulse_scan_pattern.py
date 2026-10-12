"""Foreground SolarOS probe for asynchronous pulse scanning.

Configure refresh=pulse and pulse-scan=async from telnet before launch.
This schedules image targets, not physical completions. Read actual cadence
from display mode/log show and inspect text, contrast, trails, and the seam.
"""
from solaros import gfx, time

FRAME_MS = 100
FRAMES = 120

gfx.begin()
try:
    width, height = gfx.size()
    patch = min(68, height // 3)

    def draw(x, step):
        gfx.clear(gfx.WHITE)
        gfx.color(gfx.BLACK)
        gfx.font(gfx.FONT_MONO)
        gfx.text(8, 24, "100 ms targets - watch settling and erased patches")
        gfx.text(8, 44, "step " + str(step))
        gfx.rect(2, 2, width - 4, height - 4)
        gfx.fill_rect(x, height // 2, patch, patch)
        gfx.present()

    draw(8, 0)
    time.sleep_ms(5000)  # Initial native baseline and mask initialization.
    due = time.uptime_ms()
    for frame in range(FRAMES):
        draw(8 + (frame * 8) % max(1, width - patch - 16), frame // 10)
        due += FRAME_MS
        delay = due - time.uptime_ms()
        if delay > 0:
            time.sleep_ms(delay)
    print("queued", FRAMES, "targets; inspect driver scan timings")
    time.sleep_ms(4000)  # Final target must continue settling without presents.
finally:
    gfx.end()
