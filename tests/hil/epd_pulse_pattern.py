"""SolarOS MicroPython panel probe. Run as a foreground display-shell script.

Set the display's refresh/tuning mode from telnet before launching this script.
Driver logs distinguish transfer and waveform completion; gfx.present() queues
work and does not measure physical completion. Inspect the panel for gray drift, retained
patches, wrong polarity, and a mismatch between the two controller halves.
"""
from solaros import gfx, time


def present(label):
    gfx.present()
    print("queued", label)
    time.sleep_ms(150)


gfx.begin()
try:
    width, height = gfx.size()
    patch = max(8, min(width, height) // 8)
    for background, foreground, label in (
        (gfx.WHITE, gfx.BLACK, "black on white"),
        (gfx.BLACK, gfx.WHITE, "white on black"),
    ):
        for step in range(8):
            gfx.clear(background)
            gfx.color(foreground)
            gfx.font(gfx.FONT_MONO)
            gfx.text(4, 20, label)
            gfx.rect(1, 1, width - 2, height - 2)
            x = (width - patch) * step // 7
            gfx.fill_rect(x, height // 2, patch, patch)
            present(label + " " + str(step))
    gfx.clear(gfx.WHITE)
    gfx.color(gfx.BLACK)
    gfx.text(4, 20, "Inspect background, removed patches, and panel seam")
    present("stationary")
    present("identical frame")
    # Return patches to white, then allow a visual check before shell redraw.
    time.sleep_ms(3000)
finally:
    gfx.end()
