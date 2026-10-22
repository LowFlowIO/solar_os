+++
id = "motor"
title = "Motor control"
aliases = ["drv8830"]
section = "hardware"
summary = "Control a brushed DC motor through a named I2C bus"
keywords = "drv8830 motor brushed dc voltage coast brake fault int i2c"
packages_any = ["drv8830"]
+++
# Motor control

Use `motor` to control attached brushed DC motors. The currently supported
controller is the DRV8830.

The reusable `drv8830` expansion driver controls one brushed DC motor per
controller. Multiple controllers can share a named I2C bus at distinct addresses.
Select the `drv8830` package group; it is included in the `full` flavor on
boards with expansion I2C support.

## Quick reference

```text
expansion attach drv8830 motor0 i2c=i2c0 addr=0x60
motor list
motor status motor0
motor drive motor0 2000
motor drive motor0 -2000
motor coast motor0
motor brake motor0
motor clear motor0
expansion detach motor0
```

## Wiring and attachment

Connect SDA/SCL to the selected bus, share ground, and supply the controller
and motor according to their ratings. The bus must run at 400 kHz or less.
`addr` is the seven-bit address selected by A1/A0: `0x60` through `0x68`.
An acknowledgement confirms communication; the chip has no identity register.

FAULTn is optional. To connect it to an available native input GPIO, add
`int=gpio1` (replace GPIO1 with a suitable pin on your board):

```text
expansion attach drv8830 motor0 i2c=i2c0 addr=0x60 int=gpio1
```

FAULTn is an active-low, open-drain output. Use a pull-up compatible with the
ESP32 input; the driver also enables the native input pull-up. SolarOS claims
the address and optional GPIO for the attachment. The named bus owns SDA/SCL.
No interrupt wire is required to read and clear the fault register.

Python and Lua can create the same attachment with
`solaros.expansion.attach("drv8830", "motor0", bindings)`. Use a dictionary
(Python) or table (Lua) with `i2c="i2c0"`, `addr=0x60`, and optional integer
`int=1`. Motor controls are exposed through the shell and C service below.

## Control and fault handling

Attach and successful detach put the bridge in coast/standby (outputs Hi-Z).
`drive` accepts signed millivolts: positive drives OUT1 high relative to OUT2,
negative reverses polarity, and zero coasts. Nonzero magnitudes must be
`480..5060` mV; values are rounded to the nearest supported VSET code, `6..63`.
Direction reversal passes through standby. `brake` drives both outputs high.
Coast and brake do not disconnect the motor supply.

The reported voltage is the nominal DAC setting, not a measured motor voltage
or speed. Output is limited by the available supply. Current limiting is set
by the external sense resistor, not a software current setting.

`status` reads CONTROL and FAULT and decodes `FAULT`, `OCP` (overcurrent),
`UVLO` (undervoltage), `OTS` (overtemperature), and `ILIMIT` (extended current
limiting). If `int` is wired, status also samples its active-low level. This
input is sampled on status and drive calls; there is no asynchronous fault
callback. The chip's hardware protections operate independently.

A nonzero drive command is rejected while fault bits or the wired fault input
are asserted. `clear` first coasts, then clears fault latches, leaving the
motor stopped. Read status again before resuming. Stop commands remain
available during faults. A failed control write attempts to coast and returns
the original error; a communication failure cannot guarantee a physical stop.
If detach cannot stop the bridge, it keeps the attachment and resource claims
so the operation can be retried.

## Reuse from C

Applications use the named service in `solar_os_drv8830.h`: enumerate with
`solar_os_drv8830_get()`, inspect with `solar_os_drv8830_status()`, and control
with `solar_os_drv8830_drive()`, `solar_os_drv8830_stop()`, and
`solar_os_drv8830_clear()`. The service serializes operations with detach.
The expansion registry owns attachment, bus leases and resource claims.
The low-level register driver in `drv8830.h` uses injected read/write callbacks.

Register definitions and electrical requirements follow the
[TI DRV8830 datasheet](https://www.ti.com/lit/ds/symlink/drv8830.pdf), particularly
Table 1 (VSET), Table 4 (bridge logic), Table 5 (addresses), and section 7.6.
