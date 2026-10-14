+++
id = "native-library"
title = "Native libraries"
summary = "Install reusable native libraries and manage their lifetime"
section = "api"
keywords = "native library resident module provider lease unload ABI"
packages_any = ["service_native_modules"]
agent_reference_sections = true
+++
# Native libraries

A native library supplies a reusable, versioned C API to native apps, jobs, or
firmware services. Libraries require ESP32-S3 with executable PSRAM and the
native-module service. Each loaded library has one shared instance.

## Quick reference

```text
pkg available
pkg installed
pkg unload <library-id>
pkg remove <library-id>
```

Use `pkg install <library-id>` for a library present in the signed catalog.
Installation checks the catalog signature, target, host and lifecycle ABIs,
capabilities, artifact size and hash, and the library registration. The ELF is
stored under `modules/libraries/<library-id>.elf` in the preferred volume.
`pkg installed` reports whether a library is loaded or only on storage.

Libraries are loaded when a consumer acquires their API. Acquisition reads the
installed file; it does not download code. Loaded libraries use PSRAM and remain
resident until explicitly unloaded or removed. Installed libraries are not
loaded by the boot scan. After restarting, consumers load them again as needed.

`pkg unload` releases the resident image but keeps its installed file.
`pkg remove` unloads the library and removes the file. Both refuse while a
consumer holds a lease or the library cannot finish shutdown. Unload an unused
library before reinstalling it.

## Developer reference

Include `solar_os_native_library_abi.h`. A provider calls
`solar_os_native_library_host_v1()` from its entry point and registers one
`solar_os_native_library_descriptor_v1_t`. Its ID must match the installed
module ID. Registration declares a summary, API table, and shutdown callback;
it must not start workers or acquire resources. Library lifecycle ABI 1 is
independent of the provider's own API version.

Every provider API table starts with `uint32_t abi_version, struct_size`.
Append new fields to compatible tables. Consumers request the exact provider
ABI and enough table bytes for the methods they use. Missing or incompatible
providers return an error, a zero lease, and a null API pointer.

Native consumers request `get_service("library", 1, sizeof(*libraries))` from
their host table. The returned `solar_os_native_library_service_api_v1_t`
provides `acquire`, `retain`, `release`, and `unload`. Results use
`solar_os_native_service_result_v1_t`.

```c
uint32_t lease = 0;
const void *table = NULL;
int result = libraries->acquire("my-library", MY_LIBRARY_ABI,
    sizeof(my_library_api_v1_t), &lease, &table);
if (result == SOLAR_OS_NATIVE_OK) {
    const my_library_api_v1_t *api = table;
    /* Use api; destroy its objects and stop callbacks before releasing. */
    libraries->release(lease);
}
```

Each acquire returns an independent lease. `retain` creates another lease for
a model, client, or worker that must outlive the original consumer. Keep that
lease until its calls, objects, workers, and callbacks have finished. Release
all owned leases before the consuming module exits. Lease operations are
synchronized; the provider is responsible for synchronizing its own mutable
state. The runtime supports at most 128 outstanding library leases.
Loading and unloading share native-module admission. A request blocked by
another module operation returns `SOLAR_OS_NATIVE_ERROR_INVALID_STATE` and can
be retried when that operation finishes.

Releasing a lease invalidates its token. Releasing or retaining a stale token
fails. The API pointer is valid while at least one owned lease protects it;
never call through it after releasing all such leases.

With no leases, unloading calls `shutdown`. The provider joins its workers,
releases dependencies and allocations, then returns `SOLAR_OS_NATIVE_OK`.
A failed shutdown leaves the image loaded and allows a later retry. It must
leave the API usable when refusing shutdown. Shutdown cannot load or unload
another module while the resident operation is in progress, but can release
leases on dependencies.

Firmware consumers use `solar_os_native_library_acquire`, `retain`, `release`,
and `unload` through `solar_os_native.h`; they use the same stable result codes.
These APIs do not expose arbitrary firmware symbols to external libraries.
