# EPD pixel-budget layer

`src/drivers/epd_pixel_budget.h` provides a controller-independent monochrome
target and remaining-cycle model. It owns no SPI bus, GPIO, display task,
allocation, or waveform. A panel driver supplies storage, maps physical pixel
order to indices, and encodes HOLD/DRIVE_BLACK/DRIVE_WHITE into its controller's
RAM and waveform selections.

Drivers may attach caller-owned `uint32_t[count]` active-index storage before
seeding or retargeting. Budget advancement then visits pending pixels only.
The list compacts after a successful cycle; seeded entries can remain listed
until compaction and must be checked with `epd_pixel_budget_drive()`. Reset
the model before seeding a complete native baseline. Without active storage,
the same API uses a full-map scan.

Seed every pixel after a successful native cleanup. A changed target receives
the configured cycle budget. Repeating that target preserves its remaining
budget; reversing it assigns a new budget in the new direction. Call
`epd_pixel_budget_advance()` only after a complete successful waveform.
Pixels with zero remaining cycles return HOLD. A failed waveform leaves
optical state unknown: reset/reseed from a native cleanup before continuing.

The common tuning parser handles `drive-cycles`, `pulse-frames`,
`waveform-fr`, and `cleanup-every`. Unknown keys return `ESP_ERR_NOT_FOUND`;
invalid values leave configuration unchanged. `waveform-fr` is a profile's raw
frame-rate code, not a portable frequency. Profiles may impose narrower limits.

The dual-SSD1683 5.79-inch adapter is the first consumer. It programs both
controllers, replaces both RAM planes whenever the active drive mask changes,
reuses them between identical cycles with RAM ping-pong disabled, holds seam padding
neutral, and serializes tuning with presentation. It completes pending cycles
synchronously. The common model also supports retargeting between cycles for
a future scan worker, but supplies no optical feedback or grayscale model.
The first custom activation clears native image bits from all four RAM planes.
Later writes cover the union of the current and preceding active bounds, so
old drive bits become neutral before another activation. This restricts SPI
traffic, not the controller's physical gate scan.
SSD1677 and UltraChip profiles need their own validated waveform adapters
before enabling the same controls. Matching a controller family alone does
not establish a panel's electrical or optical compatibility.

Waveform register layout follows Solomon Systech's SSD1683 Rev 1.0, sections
6.5–6.7. Dual-controller custom-LUT programming was cross-checked against
Good Display's panel sequence as published in
[Typoena's GDEY0579T93 driver](https://github.com/typoena/typewriter/blob/main/firmware/src/drivers/screen_epd.rs).
SolarOS generates its own short differential waveform; it does not reuse the
other project's full waveform table. Pulse mode explicitly programs the
panel's published source/gate rail and VCOM settings on both controllers,
including booster soft start, and disables RAM ping-pong because it writes
both planes explicitly. Pulse start, completion, and errors use the SolarOS
log ring and can be read with `log show` over a port shell.
