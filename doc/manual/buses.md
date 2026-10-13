+++
id = "buses"
title = "Named runtime buses"
section = "api"
summary = "Create and use resource-owned I2C, SPI, UART, MIDI, OneWire, and PS/2 buses"
aliases = ["bus"]
keywords = "python lua buses resource named bus create attach detach i2c spi uart midi onewire ps2 keyboard lease"
packages_any = ["service_resources"]
+++
# Named runtime buses

A named bus describes a hardware connection: I2C, SPI, UART, MIDI, 1-Wire,
or PS/2. Scripts and attached devices refer to its name instead of repeating
its wiring. SolarOS tracks which devices use it.

## Inspect from the shell

```text
expansion bus list
i2c status i2c0
i2c scan i2c0
```

Use the names shown on your device. The `io` app can create and manage buses;
[Expansion reference](expansion.reference.md) lists the wiring arguments.

## Inspect before creating

```python
import solaros

for bus in solaros.buses.list():
    print(bus)
```

Reuse a suitable registered bus instead of creating a duplicate on the same
pins. Use `attach` and `detach` for leases, and remove only a runtime bus that
has no users.

Change the clock of a board-defined or runtime-created named I2C bus without
recreating its descriptor:

```text
i2c speed i2c0 400000
i2c status i2c0
```

The new speed applies to subsequent transfers, including those from attached
devices. In `io`, select the bus and choose `Set I2C speed`.

## Transfer through the registry

Use the API for the bus type: `i2c_scan`, `spi_xfer`, `uart_read`, or
`onewire_scan`. Inspect the result and release any lease even when a transfer
fails.

## Quick reference

Prefer solaros.buses for named hardware. list and get inspect registered buses;
create_i2c, create_onewire, create_ps2, create_spi, create_uart, and create_midi create runtime buses;
attach, detach, remove manage lifecycle. Transfer families are i2c_probe or
scan or read_reg or write_reg, onewire_reset or scan or xfer, spi_xfer or read
or write, and uart_read or write. Inspect descriptors before choosing names,
pins, chip selects, or hosts.

A MIDI bus is a first-class, exclusive bus backed internally by an available
UART controller. `create_midi(name, {"tx": ..., "rx": ...})` does not take a
controller number and defaults to 31250 baud. Start `job midi` to receive and
transmit MIDI messages. Use optoisolated MIDI IN and a current-limited MIDI OUT
driver; do not wire a DIN MIDI connector directly to ESP32 GPIOs.
