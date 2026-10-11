+++
id = "native-app"
title = "Native foreground apps"
summary = "External app lifecycle, resource declarations and foreground services"
section = "api"
keywords = "native app foreground emulator apple2 gameboy suspend resume resources"
packages_any = ["service_native_modules"]
agent_reference_sections = true
+++
# Native foreground apps


Native foreground apps are trusted ESP32-S3 modules. They use the same sessions,
display ownership, keyboard shortcuts and cold-state lifecycle as built-in apps.
Emulators such as Apple II Plus are foreground apps. Their CPU cores and guest
state belong to their modules.

## Quick reference

```text
pkg installed
apple2 /sdcard/apple2 dos33.nib
session create apple2 display0 /sdcard/apple2 dos33.nib
load /sdcard/apple2.app.elf /sdcard/apple2 dos33.nib
```

## Installation and launch

Catalog entries use `type = "app"` and `lifecycle_abi = 2`. `pkg install ID`
installs an ELF under `modules/apps/ID.elf` in the preferred storage volume.
Run `ID [arguments]` from a display shell, or use
`session create ID display0 [arguments]`. `load /path/app.app.elf [arguments]`
also recognizes foreground metadata. The filename does not determine the
lifecycle of an app that declares metadata. Lifecycle-1 apps remain synchronous
commands; their code is unloaded when `main` returns.

Installed foreground apps appear in app discovery. An installed foreground app
with a built-in app's ID takes precedence while installed. Each app can own one
session. Different external apps can remain loaded in suspended sessions.
Installing or removing a running or suspended foreground app is refused.
Resource declaration changes to an app already discovered require a restart.

The package catalog has application, job and driver categories.

## Resource declaration

`solar_os_native_app_abi.h` defines the exported, pointer-free
`solar_os_native_app_manifest` record. It declares the app ID, summary, GUI or
TUI class, resumable flag, worker stack and priority, internal-memory and PSRAM
minima, event tick interval, and optional file extensions. File extensions are
space-separated lowercase suffixes such as `.gb`; opening a matching file passes
its path as the first argument. The host validates this record before executing
module code and checks it again when loading the session worker. Catalog
lifecycle and ID must match the ELF metadata.

The declared memory minima are admission checks, not reserved heaps or quotas.
Bulk guest state and executable images use PSRAM. An app owns its allocations
and frees them before returning. Worker resources are allocated on launch;
no app-specific startup hook runs at boot.

## Suspend, resume and close

`Alt+Tab` and `Alt+Right` select the next session; `Alt+Left` selects the previous
session. Display shells also provide `session fg ID`. A resumable module polls
`suspend_requested` at safe points, including while paused, and calls `suspend`.
The host joins presentation and closes foreground audio before acknowledging the
switch. Guest state, the ELF image and its worker remain allocated. Resume
restores display ownership, clears held input and allows the worker to continue.
The module rebases its timing, restores optional audio and submits a full frame.

Ctrl+Alt+Del, Ctrl+] or `session close ID` requests cooperative shutdown.
The module polls `stop_requested`, saves any state required by its app, stops its
callbacks, frees allocations and returns. The host joins presentation and audio
before unloading the image. A worker that fails to suspend or close within five
seconds causes a device restart; it is never forcibly deleted while executing
module code. Native modules run in the firmware address space.

## Foreground service

`main(argc, argv)` obtains `solar_os_native_host_v1()` and requests
`get_service("foreground", 1, required_size)`. Check the returned table and its
size before using appended methods. Calls belong to that app's worker; tables
cannot be used by another app or a shell task.

The service provides time and bounded sleep, display palette and asynchronous
frame submission, input events including release and reset, path resolution,
bounded file reads and atomic replacement, optional PCM audio, exit status,
and cooperative suspend and stop. Each app has its own input queue, presenter,
audio ownership and retained state. OS session shortcuts pass through captured
keyboards. Modules must not leave callbacks or tasks running after `main` returns.

Packed INDEX2 frames use four shades of the current display theme. The appended
synth methods accept a bounded block size and required sample rate. Callbacks
render signed 16-bit stereo PCM under a host mutex. The module uses
`synth_lock` and `synth_unlock` when accessing shared synthesizer state; the host
joins synth callbacks on suspend and close. The mutex remains available while
suspended. Failed synth startup leaves video and emulation usable without audio.

The memory service provides PSRAM allocation and optional `alloc_internal` for
hot state or caches. Internal allocation preserves the OS reserve and returns
NULL when unavailable; modules can omit caches or fall back to PSRAM. The host
also provides the current RTC register values, board-button source identity,
and presenter statistics. Standard compiler helpers and `setjmp`/`longjmp`
are exported for synchronous interpreter error handling.

## Apple II Plus

See the [Apple II Plus module](../../modules/apple2/README.md) for ROM and NIB
requirements and controls. For example, with the asset directory populated:

```text
apple2 /sdcard/apple2 dos33.nib
```

F1 saves drive 1; Alt+F1 saves drive 2. F10 pauses the guest and F11 resets it.
Disk writes remain in memory until explicitly saved. Session switches retain
unsaved disk changes and the guest's pause state.
