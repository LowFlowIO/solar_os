+++
id = "gpio.analog"
title = "GPIO, ADC, PWM, and LED APIs"
section = "hardware"
summary = "Use runtime-safe digital and analog expansion pins"
aliases = ["gpio", "adc", "pwm"]
keywords = "python lua gpio pin digital led adc analog pwm input output pull read write voltage duty"
packages_any = ["service_gpio", "service_adc", "service_pwm"]
+++
# GPIO, ADC, PWM, and LED APIs

SolarOS exposes only runtime-safe pins for the active board. Never copy a GPIO
number from another ESP32 board or assume that a header pin is unclaimed.

## Discover first

```python
import solaros

for pin in solaros.gpio.pins():
    print(pin)
```

Check `allowed(pin)` before configuring a pin. Release a claimed pin when the
script is done so another service can use it.

## Digital output

```python
pin = 1
if solaros.gpio.allowed(pin):
    solaros.gpio.mode(pin, solaros.gpio.OUTPUT)
    try:
        solaros.gpio.write(pin, 1)
    finally:
        solaros.gpio.release(pin)
```

Use the documented mode and pull constants. Do not replace them with guessed
strings or integers.

## Quick reference

Inspect solaros.gpio.pins() and allowed(pin) before use; never invent safe GPIO
numbers. GPIO offers mode or configure, read, write, and release plus INPUT,
OUTPUT and pull constants. solaros.led offers status, set, on, off, toggle.
solaros.adc offers pins and read. solaros.pwm offers providers, status,
set(output, frequency, duty_percent), pulse(output, frequency, pulse_us), and
off. Outputs use provider:number; integers select native GPIOs. Providers
enforce their shared-frequency constraints. For calibrated
positional outputs, use [the servo service](servo.md). APIs are package-gated.
