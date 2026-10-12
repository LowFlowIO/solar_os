+++
id = "app.tiny386"
title = "Tiny386 application"
section = "app"
summary = "Boot PC floppy and hard-disk images with the external 386 emulator"
aliases = ["tiny386"]
keywords = "tiny386 386 pc emulator dos freedos bios vga floppy ide hard disk img suspend resume"
packages_any = ["service_native_modules"]
agent_reference_sections = true
+++
# Tiny386

Tiny386 is an external foreground app for ESP32-S3 devices with PSRAM,
graphics, and native-module support. It emulates a 386 PC with 2 MiB RAM,
256 KiB VGA RAM, PS/2 keyboard, timers, RTC, a floppy drive, and an IDE hard
drive. SeaBIOS and
the VGA BIOS are embedded in the module. Guest disk images are supplied
separately. Networking, mouse, and audio are not supported.

## Quick reference

```text
tiny386 /sdcard/tiny386/dos.img
tiny386 /sdcard/tiny386/dos.img --hdd /sdcard/tiny386/harddisk.img
tiny386 --hdd /sdcard/tiny386/bootable-harddisk.img
session create tiny386 display0 /sdcard/tiny386/dos.img
session close ID
```

## Installation and images

Copy `solaros_native_tiny386.app.elf` to `modules/apps/tiny386.elf` in the
preferred storage volume. The firmware must support the foreground
disk-image service methods. The app requires at least 5 MiB free PSRAM
and reserves a 16 KiB worker stack; startup can fail if memory is fragmented
or other apps retain large allocations.

Use a bootable raw floppy `.img` of 720 KiB, 1.2 MiB, 1.44 MiB, or 2.88 MiB.
An `.img` extension alone does not distinguish a floppy from a hard-disk image.
Use `--hdd` to attach one raw IDE disk with 512-byte sectors, smaller than 2 GiB.
Disk data is accessed directly from storage. Keep an original copy when using
a guest that modifies its disks. A bootable hard disk can start without a
floppy; with both attached, SeaBIOS tries the floppy first.

Create a formatted 32 MiB hard disk with
`disk create /sdcard/tiny386/harddisk.img 32`. It is ready for files but does
not contain a bootable OS. Boot a DOS floppy to install DOS onto the hard drive.
For FreeDOS, run these commands from the booted floppy:

```text
FDISK /MBR
SYS C:
```

`FDISK /MBR` installs the master boot code while keeping the existing partition
and files. `SYS C:` installs the DOS boot sector and system files. The partition
created by `disk create` is already marked active. Close the emulator, then
launch it with only `--hdd` to boot from the hard disk.
See [disk images](storage.md#disk-images) to copy files into it from SolarOS.

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
Suspend retains the CPU, guest RAM, VGA state, and exclusive disk claims,
pauses guest time, and releases presentation resources. Resume clears held
keys and redraws the screen.

Guest writes update and sync the image file before the disk operation returns;
storage must be writable. Read, write, sync, or close failures are reported to
the launching shell. Close the guest cleanly before removing storage or
mounting its image in SolarOS. A suspended guest still owns its disks. Power
loss during a write or a guest's incomplete filesystem update can lose data.

See the [foreground app contract](native-app.md) and
[module build and BIOS source instructions](https://github.com/nilseuropa/solar_os/blob/main/modules/tiny386/README.md).
