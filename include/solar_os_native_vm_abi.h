#pragma once
#include "solar_os_native_app_abi.h"
/* Source compatibility for existing emulator binaries. New modules declare
 * lifecycle-2 app metadata and request the foreground service. */
#define SOLAR_OS_NATIVE_VM_SERVICE "vm"
#define SOLAR_OS_NATIVE_VM_ABI SOLAR_OS_NATIVE_FOREGROUND_ABI
#define SOLAR_OS_NATIVE_VM_KEY_PRESS SOLAR_OS_NATIVE_FOREGROUND_KEY_PRESS
#define SOLAR_OS_NATIVE_VM_KEY_RELEASE SOLAR_OS_NATIVE_FOREGROUND_KEY_RELEASE
#define SOLAR_OS_NATIVE_VM_KEY_REPEAT SOLAR_OS_NATIVE_FOREGROUND_KEY_REPEAT
typedef solar_os_native_foreground_key_v1_t solar_os_native_vm_key_v1_t;
typedef solar_os_native_foreground_api_v1_t solar_os_native_vm_api_v1_t;
