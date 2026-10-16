# SPI Display Link library

Portable C11 implementation of SPI Display Link Protocol (SDLP v1).
It includes explicit byte serialization, CRC, descriptors, a host client, and
a peripheral state machine with request replay and asynchronous presentation.
It has no SDK, RTOS, allocation, framebuffer, or GPIO dependency. The MIT
license permits use in other embedded projects.

## Build and test

```sh
cmake -S protocol/display_link -B /tmp/display-link-build -DSDLP_BUILD_TESTS=ON
cmake --build /tmp/display-link-build
ctest --test-dir /tmp/display-link-build --output-on-failure
python3 -m unittest discover -s tests -p test_display_link.py
```

The Python runner adds AddressSanitizer and UBSan. A normal library consumer
compiles `src/sdlp.c` and adds `include/` to its include path, or uses the CMake
`display_link` target. Both roles are in the same source; unused functions can
be removed by the toolchain's function-section garbage collection.

## Host adapter

Fill `sdlp_transport_t` with a monotonic microsecond clock, a delay callback,
and a bounded 256-byte full-duplex SPI exchange. The exchange callback owns CS,
configured minimum CS-high spacing, optional READY, and shared-bus locking.
It must respect the absolute deadline and return with CS inactive and the bus
released. Host APIs return a wire result or a negative local error.

Initialize `sdlp_host_t`, discover before an absolute boot deadline, then call
`sdlp_host_configure()` with a fresh nonzero session ID, an accepted format, and
the host's clock ceiling. Discovery uses bootstrap timing. The selected clock
and gap apply after successful configuration.

Use BEGIN_FRAME, BEGIN_RECT, and sequential writes of at most 208 bytes.
Writes can cross pixel and row boundaries. PRESENT starts presentation;
`sdlp_host_wait_frame()` waits for the corresponding completion or failure.
`sdlp_host_command()` also exposes abort, status, and optional controls.

The client retries identical request bytes within a bounded command deadline.
After a transport timeout the outcome is unknown, and the client invalidates
its configured state. Rediscover and create a new session before sending a
complete replacement image. A pending presentation may temporarily reject
configuration with BUSY; each new configuration attempt needs a fresh ID.
The client is synchronous and is not thread-safe: its owner serializes calls.

## Peripheral adapter

Initialize `sdlp_device_t` with an immutable descriptor, nonzero boot ID that
changes after protocol state loss, and `sdlp_backend_t` callbacks. State and
replay storage are caller-owned. The backend owns its display representation.

Arm TX with `sdlp_device_idle()`. Receive one complete cell per CS assertion;
after CS rises, pass only a correctly sized, non-timed-out cell to
`sdlp_device_receive()`. When it returns true, queue the returned response for
a later transfer. When false, keep the previous TX cell. The state machine
checks CRC, padding, command bounds, session/boot identity, and replay before
passing accepted bytes to the backend. The adapter rejects short/overlong
hardware transactions before calling it.

Call `sdlp_device_tick()` regularly even without host traffic. `present()` must
start bounded asynchronous work; `poll_present()` returns BUSY until it has
finished. Other callbacks must finish soon enough for the transport's stated
rearm interval. A platform with slow callbacks needs independent transport
servicing and serialized state-machine work. The library itself does not
create workers or interrupts and is not reentrant.

Backend callbacks return SDLP wire results. BAD_ARGUMENT, BAD_STATE, BUSY,
UNSUPPORTED, and NO_MEMORY must leave the requested operation unapplied.
IO_ERROR or TIMEOUT may indicate partial downstream effects and require a new
configuration/full repaint. Never return OK for work that was dropped.

## Reference peripheral

The sibling `pico_display_link` repository vendors these files for independent
builds. Its `scripts/sync_protocol.py` explicitly updates that copy. Run its
native protocol/framebuffer tests and target builds after synchronization.
