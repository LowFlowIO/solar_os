+++
id = "native-vm"
title = "Native emulator compatibility"
summary = "Legacy emulator files use the foreground app runtime"
section = "api"
keywords = "native app foreground emulator apple2 gameboy suspend resume resources"
packages_any = ["service_native_modules"]
agent_reference_sections = true
+++
# Native emulator compatibility


## Quick reference

Emulators use the [native foreground app lifecycle](native-app.md).
Existing `.vm.elf` files can still be opened with `load`; new modules declare
`type = "app"`, lifecycle ABI 2 and a foreground manifest.
