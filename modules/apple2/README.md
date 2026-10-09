# Apple II Plus VM module

External SolarOS VM based on [Reinette II Plus](https://github.com/ArthurFerreira2/reinette-II-plus).
Upstream snapshot: `b272efd` (2023-09-10). MIT notices and the license are retained
under `main/vendor`. No Apple ROMs or game/disk images are bundled.

The module emulates an Apple II Plus with 64 KiB RAM including the language
card, 40-column text, lores/hires and mixed graphics, two Disk II drives,
paddles and the speaker. It supports NIB disk images. Undocumented CPU opcodes
act as NOPs. The font is derived from upstream `assets/font-normal.bmp`.

Build with ESP-IDF 5.5 for ESP32-S3:

```sh
idf.py set-target esp32s3
idf.py build
cmake --build build --target typed_elf
```

Artifact: `build/solaros_native_apple2.vm.elf`. Copy it to storage and run:

```text
load /sdcard/solaros_native_apple2.vm.elf /sdcard/vm/apple2 dos33.nib
```

Use `pkg install apple2` to install from the module catalog, then
`apple2 /sdcard/vm/apple2 dos33.nib` to start the installed module.
The asset directory requires `appleII+.rom` (12288 bytes) and `diskII.rom`
(256 bytes). NIB images must be exactly 232960 bytes. A second disk is optional.
The asset directory can be relative to the launching shell's current directory:
from `/sdcard`, use `apple2 vm/apple2 dos33.nib`; from `/sdcard/vm/apple2`,
use `apple2 . dos33.nib`. Relative disk names use the resolved asset directory.
The firmware must provide the VM `resolve_path` callback.

F1 saves drive 1; Alt+F1 saves drive 2. F2 shows status, F3 toggles disk
acceleration, F4 mutes, F10 pauses, F11 resets the guest, and F12 shows help.
Ctrl+Alt+Del or Ctrl+] exits to the SolarOS shell. Exit discards unsaved disk
changes. The module requires firmware providing the VM `stop_requested` callback.
See [native VM reference](../../doc/manual/native-vm.md).

`Alt+Tab` or `Alt+Right` switches to the next SolarOS session; `Alt+Left`
switches to the previous session. Resume with these shortcuts or `session fg ID`.
Switching pauses the guest and releases display, keyboard and audio resources;
CPU, RAM and disk state, including unsaved changes, remain allocated. F10 pause
is retained across session switches. The firmware must provide `enable_suspend`,
`suspend_requested` and `suspend`.

The module uses the SolarOS VM host for display, keyboard, storage, timing
and optional audio. Text and overlays follow terminal colors on color displays
and use black on white on monochrome displays. Graphics retain their Apple II
palettes. The firmware must provide `text_colors` and `video_open_ex`.
Changed frames are submitted asynchronously at up to 25 Hz.
F2 includes guest speed, interpreter throughput and frame-rendering time.
