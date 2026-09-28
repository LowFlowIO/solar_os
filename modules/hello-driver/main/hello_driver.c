#include <stddef.h>

#include "solar_os_native_driver_abi.h"

static unsigned attached_devices;

static int hello_driver_attach(const char *device_name)
{
    if (device_name == NULL || device_name[0] == '\0') {
        return 1;
    }
    attached_devices++;
    return 0;
}

static int hello_driver_detach(const char *device_name)
{
    if (device_name == NULL || device_name[0] == '\0' ||
        attached_devices == 0U) {
        return 1;
    }
    attached_devices--;
    return 0;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    const solar_os_native_driver_host_api_v1_t *host =
        solar_os_native_driver_host_v1();
    if (host == NULL ||
        host->abi_version != SOLAR_OS_NATIVE_DRIVER_LIFECYCLE_ABI ||
        host->struct_size < sizeof(*host) ||
        host->register_driver == NULL) {
        return 1;
    }
    static const solar_os_native_driver_descriptor_v1_t descriptor = {
        .abi_version = SOLAR_OS_NATIVE_DRIVER_LIFECYCLE_ABI,
        .struct_size = sizeof(descriptor),
        .id = "hello-driver",
        .summary = "zero-resource native driver acceptance module",
        .attach = hello_driver_attach,
        .detach = hello_driver_detach,
    };
    return host->register_driver(&descriptor);
}
