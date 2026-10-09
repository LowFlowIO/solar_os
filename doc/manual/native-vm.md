+++
id = "native-vm"
title = "Native virtual machines"
section = "api"
summary = "External emulator modules using SolarOS display, input, storage, timing and audio"
keywords = "vm emulator apple2 reinette native elf restart"
packages_any = ["service_native_modules"]
agent_reference_sections = true
+++
# Native virtual machines

VM modules run native ESP32-S3 code and emulate a guest machine. They require
PSRAM and a display shell. The VM host provides display, keyboard, storage,
timing and optional audio services.

## Quick reference

```text
pkg installed
apple2 /sdcard/vm/apple2 dos33.nib
load /sdcard/apple2.vm.elf /sdcard/vm/apple2 dos33.nib
```

Run an installed VM by its module ID, or use `load` with a `.vm.elf` path,
followed by up to seven module arguments. Module arguments are passed literally; use resolved
absolute storage paths where the module requires them. Native packages use
`type="vm"`, lifecycle ABI 1 and artifacts ending in `.vm.elf`. They are
installed under `modules/vms` in the preferred storage volume and launch only
on request. `pkg installed` and `pkg remove` include VM modules.

Ctrl+Alt+Del, Ctrl+] or closing/suspending a VM requests cooperative shutdown.
The host releases its resources and returns to the shell when the module returns.
If shutdown does not finish within five seconds, the host restarts the device.
Save guest disk changes before exiting. VM modules run trusted native code in
the firmware's address space.

## Host interface

Modules import `solar_os_native_host_v1`, check its version and size, and obtain
the `vm` ABI-1 table defined in `solar_os_native_vm_abi.h` through `get_service`.
The manifest must declare `minimum_host_api_size=24`. This table is available
only on the VM worker; command-style app modules and resident jobs cannot use it.
Allocate module state through the `memory` PSRAM table.

`video_open` accepts a width, height, 256-entry RGB565 palette and preferred
presentation rate. Frames are tightly packed INDEX8 rows. `present` copies the
pixels before returning. The asynchronous presenter fits frames to the display
while preserving their aspect ratio and supports monochrome fallback; emulated
timing must remain independent of presentation.

`video_open_ex` also accepts the palette index used for display margins;
`video_open` uses index 15. Direct color presentation preserves the supplied
palette. `text_colors` returns the terminal foreground and background in
RGB565, including terminal inversion, for color targets. On monochrome targets
it returns black and white; monochrome presentation applies terminal polarity.

`read_key` provides SolarOS logical keys, USB HID usage when available, modifiers
and press/release/repeat actions. The host captures the first ready keyboard
with structured key events, including one connected after launch, before
desktop shortcuts. A reset marker precedes subsequent keys after queue
overflow or a captured keyboard disconnect; clear held guest controls on reset.
The capture is released when the module returns. `time_us` is monotonic;
`sleep_ms` always yields at least one scheduler tick and caps a request at one
second. `restart` restarts the device.

Storage calls accept absolute paths. `file_size` reports a regular file's size;
`read_file` reads up to 65536 bytes at an offset and reports the actual count.
`replace_file` writes a complete file of at most 1 MiB to a synced sibling and
then replaces the destination using the storage service.

Audio is optional. `audio_open` returns the native sample rate and channel count
for S16LE PCM, or reports unavailable/busy output. `audio_write` accepts up to
4096 interleaved frames. Calls can block on output. Close audio on the opening
worker. The host respects the global output volume.

The VM host does not retain module callbacks. A module must free its own allocations
before returning. The host joins its presenter and closes audio before its
foreground state is released. Call `set_exit_code` before returning because the
underlying ELF loader does not propagate C `main`'s return value.

Poll `stop_requested` regularly, including while paused, and return after
cleanup when it reports true. The ABI-1 table grows by appending fields;
request the minimum table size needed through `get_service`. Modules using an
older table can run, but must implement cooperative shutdown to return to the
shell without a restart.

## Apple II Plus

The `apple2` module uses Reinette II Plus and its puce6502 CPU core. It emulates
an Apple II Plus with 64 KiB RAM including the language card, 40-column text,
lores/hires and mixed graphics, two Disk II drives, paddles and the speaker.
It does not emulate an enhanced IIe. Undocumented CPU opcodes act as NOPs.

Supply these files in an asset directory:

| File | Exact size |
| --- | --- |
| `appleII+.rom` | 12288 bytes |
| `diskII.rom` | 256 bytes |
| Optional `.nib` disk images | 232960 bytes each |

ROMs and disk images are supplied separately. Disk names may be relative to
the asset directory or absolute paths. Without a disk, press F11 to reach the
ROM prompt. Guest text, mixed-mode text rows, status and help use the terminal
colors on color displays. Monochrome displays use black-on-white text.
Lores and hires graphics retain their Apple II palettes.

| Key | Action |
| --- | --- |
| F1 / Alt+F1 | Save modified drive 1 / drive 2 |
| F2 | Toggle status: guest PC, drive, track, disk-read count and performance |
| F3 | Toggle disk acceleration |
| F4 | Toggle speaker mute |
| F10 | Pause/resume guest execution |
| F11 | Warm reset |
| F12 | Toggle help |
| Ctrl+Alt+Del / Ctrl+] | Exit to SolarOS shell |
| Keypad 1/3 and 5/2 | Paddle horizontal and vertical extremes |
| Ctrl, Alt, Shift | Guest paddle buttons 0, 1, 2 |

Guest writes modify the in-memory disk. F1 saves the complete image; exiting or
restarting does not save it automatically. Unsupported or busy audio leaves the guest
running silently.

Disk acceleration is enabled by default. The guest runs without wall-clock
throttling while a mounted drive's motor is active. Speaker output is muted
during accelerated disk access. F3 switches to normal timing and speaker output.

The F2 status shows `FAST` during accelerated disk access. `SPD` is guest cycles
per elapsed second, and `CPU` is guest cycles per second spent executing the
interpreter, both in MHz. `VID` is the percentage of elapsed time spent rendering
and submitting guest frames. Measurements update once per second. Interpreter
time includes interruptions by other tasks.
