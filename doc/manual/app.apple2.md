+++
id = "app.apple2"
title = "Apple II Plus application"
section = "app"
summary = "Run Apple II Plus software with the external foreground module"
aliases = ["apple2"]
keywords = "apple2 apple ii emulator rom nib disk save pause resume speaker paddles"
packages_any = ["service_native_modules"]
agent_reference_sections = true
+++
# Apple II Plus

Apple II Plus is an external foreground app for ESP32-S3 devices with PSRAM,
graphics, and native-module support. It emulates a 6502, 64 KiB RAM including
the language card, 40-column text, lores/hires and mixed graphics, two Disk II
drives, keyboard, paddles, and speaker. Undocumented CPU opcodes act as NOPs.
No ROMs or disk images are bundled.

## Quick reference

```text
pkg install apple2
apple2 /sdcard/apple2 dos33.nib
session create apple2 display0 /sdcard/apple2 dos33.nib
```

## Installation and assets

Install from a compatible module catalog with `pkg install apple2`, or copy
`solaros_native_apple2.app.elf` to `modules/apps/apple2.elf` in the preferred
storage volume. The app appears in the installed app registry.

Choose an asset directory and provide these files:

| File | Required size |
|---|---:|
| `appleII+.rom` | 12288 bytes |
| `diskII.rom` | 256 bytes |
| Each `.nib` disk image | 232960 bytes |

Both ROM files are required. One or two NIB disks can be mounted at launch;
launching without a disk is also supported.

## Launch

```text
apple2 <asset-directory> [drive1.nib] [drive2.nib]
apple2 /sdcard/apple2 dos33.nib data.nib
load /sdcard/solaros_native_apple2.app.elf /sdcard/apple2 dos33.nib
```

The asset directory can be relative to the launching shell's current directory.
From `/sdcard/apple2`, use `apple2 . dos33.nib`. Relative disk filenames use
the asset directory. Text and status overlays follow the terminal theme;
graphics use Apple II palettes. Changed frames are presented at up to 25 Hz.
Speaker output is optional; emulation continues when audio is unavailable.

## Controls

- `F1` saves drive 1; `Alt+F1` saves drive 2.
- `F2` toggles guest speed and performance information.
- `F3` toggles disk acceleration and `F4` toggles mute.
- `F10` pauses; `F11` resets the guest; `F12` toggles help.
- Numeric keypad `1`/`3` controls the horizontal paddle and `5`/`2` the
  vertical paddle. Releasing a key centers that paddle.
- Ctrl, Alt, and Shift control the three paddle buttons.
- Ctrl+Alt+Del, Ctrl+], or `session close ID` closes the app.

Letters are converted to uppercase for the guest. Arrow keys, Backspace, Enter,
and Ctrl combinations are translated to Apple II keyboard input.

## Sessions and disk saves

Use `Alt+Tab` or `Alt+Right` for the next SolarOS session and `Alt+Left` for
the previous session. `session fg ID` also selects a session. Switching retains
guest CPU, RAM, mounted disks, unsaved writes, and the pause state while
releasing display and audio. Resume clears held controls and rebases timing.

Disk writes remain in memory until saved with `F1` or `Alt+F1`. Saving atomically
replaces the corresponding NIB file and requires writable storage. Closing the
app discards unsaved disk changes.

See the [foreground app contract](native-app.md) and
[module build instructions](../../modules/apple2/README.md).
