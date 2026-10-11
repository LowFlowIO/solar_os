+++
id = "app.tiny386"
title = "Tiny386 application"
section = "app"
summary = "Boot PC floppy images with the external 386 emulator"
aliases = ["tiny386"]
keywords = "tiny386 386 pc emulator dos freedos bios vga floppy img suspend resume"
packages_any = ["service_native_modules"]
agent_reference_sections = true
+++
# Tiny386

Tiny386 is an external foreground app for ESP32-S3 devices with PSRAM,
graphics, and native-module support. It emulates a 386 PC with 2 MiB RAM,
256 KiB VGA RAM, PS/2 keyboard, timers, RTC, and a floppy drive. SeaBIOS and
the VGA BIOS are embedded in the module. Guest disk images are supplied
separately. Networking, mouse, audio, and hard disks are not supported.

## Quick reference

```text
tiny386 /sdcard/tiny386/dos.img
session create tiny386 display0 /sdcard/tiny386/dos.img
session close ID
```

## Installation and images

Copy `solaros_native_tiny386.app.elf` to `modules/apps/tiny386.elf` in the
preferred storage volume. The firmware must support the foreground
`video_dimensions` service method. The app requires at least 5 MiB free PSRAM
and reserves a 16 KiB worker stack; startup can fail if memory is fragmented
or other apps retain large allocations.

Use a bootable raw floppy `.img` of 720 KiB, 1.2 MiB, 1.44 MiB, or 2.88 MiB.
An `.img` extension alone does not distinguish a floppy from a hard-disk image.
The entire image is loaded into PSRAM. Keep an original copy when testing a
guest that modifies its disk.

## Display and controls

The app queries the selected foreground display and allocates a matching
frame. Text uses the guest VGA font, character attributes, and cursor. Cell
size depends on the number of guest columns and rows: an 80-column screen on
a 480-pixel display has six pixels per character. Graphics retain their aspect
ratio. Guest RTC starts at 2000-01-01.

Letters, punctuation, Ctrl and Shift combinations, function keys, and
navigation keys go to the guest PS/2 keyboard. SolarOS session shortcuts
retain their normal function. Ctrl+] or `session close ID` closes the app.

## Sessions and disk saves

`Alt+Tab`, `Alt+Right`, `Alt+Left`, and `session fg ID` switch sessions.
Suspend retains the CPU, guest RAM, VGA state, and unsaved floppy changes,
pauses guest time, and releases presentation resources. Resume clears held
keys and redraws the screen.

Guest writes remain in RAM until the app closes. Closing atomically replaces
the image file and requires writable storage. A save error is reported to the
launching shell. Reset or loss of power before close loses unsaved writes.

See the [foreground app contract](native-app.md) and
[module build and BIOS source instructions](../../modules/tiny386/README.md).
