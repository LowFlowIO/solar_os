# Tiny386 foreground app

External ESP32-S3 SolarOS app using [tiny386](https://github.com/hchunhui/tiny386)
at `9ed786b4dd9698306dccdb7595858dee10949994`. BSD-3-Clause and upstream MIT
notices are retained in `main/vendor`. The port uses SolarOS memory, timing,
display, input and file services; it initializes no board peripherals.

The app emulates a 386 PC with 2 MiB RAM, 256 KiB VGA RAM, PS/2 keyboard,
timer, RTC and floppy drive. The display is rendered at 640x400 and fitted to
the SolarOS display. This version has no guest networking, mouse or audio.
Guest RTC starts at 2000-01-01. It supports 720 KiB, 1.2 MiB, 1.44 MiB and
2.88 MiB raw floppy images. Each image is loaded into PSRAM. Guest writes remain
in RAM until the app closes, when the image is atomically replaced. A save error
is reported. Keep a copy of images whose original contents must be preserved.
Hard-disk images and streamed sector access are not supported by this version.

## Build and launch

Build with ESP-IDF 5.5 for ESP32-S3:

```sh
idf.py set-target esp32s3
idf.py build
cmake --build build --target typed_elf
```

The BIOS and VGA BIOS are embedded in the ELF; no separate ROM files are
required. Copy `build/solaros_native_tiny386.app.elf` to storage and run:

```text
load /sdcard/solaros_native_tiny386.app.elf /sdcard/tiny386/dos.img
```

For installed app discovery, copy the artifact to
`/sdcard/modules/apps/tiny386.elf`, then run:

```text
tiny386 /sdcard/tiny386/dos.img
```

The manifest declares a resumable GUI app, a 16 KiB worker stack, priority 1,
8 KiB free internal-memory minimum and 5 MiB free PSRAM minimum. These are
admission checks; allocation failures return an error and release guest state.

Keyboard events go to the guest PS/2 keyboard. SolarOS session shortcuts switch
away normally. Suspend retains the CPU, RAM and unsaved floppy changes, pauses
guest timers, and releases foreground resources. Resume clears held keys and
redraws the guest screen. Ctrl+] or `session close ID` closes the app and saves
floppy changes before releasing allocations.

## BIOS source and licenses

The bundled ROMs use [SeaBIOS](https://github.com/coreboot/seabios) revision
`81ec9ec0bcf45df11fb7f98339ec9036b546fca0`, tiny386's BIOS patches and the
configuration under `bios/`. `bios/seabios-source.tar.xz` contains the
corresponding unmodified source; `bios/tiny386.patch` contains the changes.
SeaBIOS license texts are included as `bios/COPYING` and `bios/COPYING.LESSER`.
Distribute these sources, patches, configuration and notices with the ROMs.

Rebuild the ROMs with a native x86 GCC/binutils toolchain, Python 3 and make:

```sh
python3 -B scripts/build_bios.py
```

The build uses a fixed version string. The resulting ROM bytes depend on the
compiler and binutils versions. `scripts/embed_bios.py` converts the bundled
ROMs into C data during the module build. No DOS, Windows or other guest disk
image is bundled.
