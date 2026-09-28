#include <stdbool.h>
#include <stdint.h>

#include "solar_os_native_job_abi.h"

static bool running;
static uint32_t tick_count;

static int hello_job_start(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    tick_count = 0U;
    running = true;
    return 0;
}

static void hello_job_stop(void)
{
    running = false;
}

static bool hello_job_tick(uint32_t now_ms)
{
    (void)now_ms;
    if (running) {
        tick_count++;
    }
    return false;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    const solar_os_native_job_host_api_v1_t *host =
        solar_os_native_job_host_v1();
    if (host == NULL ||
        host->abi_version != SOLAR_OS_NATIVE_JOB_LIFECYCLE_ABI ||
        host->struct_size < sizeof(*host) ||
        host->register_job == NULL) {
        return 1;
    }
    static const solar_os_native_job_descriptor_v1_t descriptor = {
        .abi_version = SOLAR_OS_NATIVE_JOB_LIFECYCLE_ABI,
        .struct_size = sizeof(descriptor),
        .id = "hello-job",
        .summary = "native module lifecycle acceptance job",
        .tick_interval_ms = 250U,
        .tick_deadline_ms = 10U,
        .start = hello_job_start,
        .stop = hello_job_stop,
        .tick = hello_job_tick,
    };
    return host->register_job(&descriptor);
}
