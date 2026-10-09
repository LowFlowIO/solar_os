# PWM providers and servo bindings

The PCA9685 implementation provides a concrete I2C example:
`drivers/pca9685.c` owns register transfers and timing, and
`services/solar_os_pca9685.c` adapts its channels to this provider contract.
Its per-device metadata is allocated on attachment with external memory
preferred. Its driver attachment mutex remains in internal memory; ordinary
PWM calls use the provider mutex already owned by the generic service.

`solar_os_pwm.h` defines the PWM provider contract. The generic service owns
output reservations and protects driver lifetime. Providers own hardware
allocation, frequency groups, precision, supported frequency ranges, and
physical transfers. The servo service binds `solar_os_pwm_output_t` to
calibration, limits, inversion, and a runtime owner.

The native backend registers the built-in `native` provider. Its logical output
IDs are GPIO numbers, independent of the LEDC channel allocated when opened.
GPIO-oriented C helpers remain wrappers around that provider. The generic
service and servo service can be built without the native backend.

## Expansion attachment

An expansion driver follows this sequence:

1. Claim its bus/address and any control lines through the expansion resources.
2. Initialize the controller with outputs disabled.
3. Register a uniquely named `solar_os_pwm_registration_t` with its operations
   and driver context. Registering does not create servo objects or start pulses.
4. On detach, call `solar_os_pwm_unregister(name)` before freeing driver context.
   Active general outputs, reserved outputs, and in-flight callbacks prevent
   unregistering. A disabled servo still reserves its output.
5. After unregister succeeds, deinitialize the hardware and release resources.
   If hardware cleanup fails, retain the context/resources required for retry.

The provider's context and operations must remain valid until unregister
succeeds. Registration copies names and the operations table. The native
provider is initialized before expansion registration and cannot be detached.

## Operations

`output_count` and `get_output` enumerate usable logical output IDs and status.
Each record identifies its `frequency_group`; group numbers are local to the
provider. A controller with one common frequency reports one group. An adapter
with multiple independent timers can expose multiple groups. Use `pin = -1`
when an output has no direct GPIO mapping.

`open` reserves hardware at the requested frequency with no pulses. `set`
starts or updates general PWM, using percentage or microsecond pulse width.
`write_pulse` commands a reserved output. `disable` stops pulses while retaining
hardware and timer reservations. `close` releases a reserved output; `stop`
releases general PWM. Backend callbacks validate output IDs, frequency ranges,
and compatibility with other outputs sharing hardware. They report actual
quantized frequency, pulse width, and resolution in output status.

An unsuccessful operation must retain existing reservations and commanded state.
An unsuccessful initial open or set must undo newly acquired hardware/resources.
In particular, a frequency change must not silently retime other users. Backends
should restore existing hardware settings on an unsuccessful update.

The service serializes callbacks per provider. Its global registry lock is not
held across callbacks. References cover callers waiting for the operation lock,
so detach cannot free their context. Callbacks must not re-enter their own
provider through PWM or a servo bound to it. The servo service similarly holds
only the affected servo's operation lock across I/O.

Both services use static, bounded state and create no worker tasks. The limits
are four providers, 32 active/reserved PWM outputs, and 16 named servo objects.
Native PWM independently has four managed LEDC channels on one timer.
Task-only registry and servo metadata uses external BSS when the build supports
it. Mutex storage and the native PWM driver's state remain in internal SRAM.
