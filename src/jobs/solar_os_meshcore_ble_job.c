#include "solar_os_meshcore_ble_job.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_ble.h"
#include "solar_os_jobs.h"
#include "solar_os_log.h"
#include "solar_os_meshcore_ble.h"
#include "solar_os_shell_io.h"
#include "solar_os_task.h"

#define MESHCORE_BLE_LOOP_DELAY_MS 20U
#define MESHCORE_BLE_STOP_WAIT_MS 6000U

static const char *TAG = "meshcore_ble_job";

typedef struct {
    volatile bool stop_requested;
    volatile bool task_done;
    TaskHandle_t task;
} meshcore_ble_job_state_t;

static meshcore_ble_job_state_t meshcore_ble_job;

static bool parse_addr_type(const char *text, uint8_t *addr_type)
{
    if (text == NULL || addr_type == NULL) return false;
    if (strcmp(text, "public") == 0) *addr_type = SOLAR_OS_BLE_ADDR_PUBLIC;
    else if (strcmp(text, "random") == 0) *addr_type = SOLAR_OS_BLE_ADDR_RANDOM;
    else if (strcmp(text, "rpa_public") == 0) *addr_type = SOLAR_OS_BLE_ADDR_PUBLIC_IDENTITY;
    else if (strcmp(text, "rpa_random") == 0) *addr_type = SOLAR_OS_BLE_ADDR_RANDOM_IDENTITY;
    else return false;
    return true;
}

static const char *addr_type_name(uint8_t addr_type)
{
    switch (addr_type) {
    case SOLAR_OS_BLE_ADDR_PUBLIC: return "public";
    case SOLAR_OS_BLE_ADDR_RANDOM: return "random";
    case SOLAR_OS_BLE_ADDR_PUBLIC_IDENTITY: return "rpa_public";
    case SOLAR_OS_BLE_ADDR_RANDOM_IDENTITY: return "rpa_random";
    default: return "unknown";
    }
}

static bool parse_pin(const char *text, uint32_t *pin)
{
    if (text == NULL || pin == NULL || strlen(text) != 6U) return false;
    uint32_t value = 0U;
    for (size_t i = 0; i < 6U; i++) {
        if (!isdigit((unsigned char)text[i])) return false;
        value = value * 10U + (uint32_t)(text[i] - '0');
    }
    *pin = value;
    return true;
}

static bool parse_args(int argc, char **argv, uint8_t bda[6], uint8_t *addr_type,
                       bool *pin_supplied, uint32_t *pin)
{
    int first = 0;
    if (argc > 0 && argv != NULL && argv[0] != NULL &&
        strcmp(argv[0], solar_os_meshcore_ble_job.name) == 0) {
        first = 1;
    }
    const int remaining = argc - first;
    if (argv == NULL || (remaining != 2 && remaining != 3) ||
        !solar_os_ble_parse_address(argv[first], strlen(argv[first]), bda) ||
        !parse_addr_type(argv[first + 1], addr_type)) {
        return false;
    }
    *pin_supplied = remaining == 3;
    *pin = 0U;
    return !*pin_supplied || parse_pin(argv[first + 2], pin);
}

static void meshcore_ble_task(void *arg)
{
    (void)arg;
    while (!meshcore_ble_job.stop_requested) {
        solar_os_meshcore_ble_loop_once();
        solar_os_meshcore_ble_note_stack_watermark(
            (uint32_t)uxTaskGetStackHighWaterMark(NULL));
        vTaskDelay(pdMS_TO_TICKS(MESHCORE_BLE_LOOP_DELAY_MS));
    }
    meshcore_ble_job.task_done = true;
    meshcore_ble_job.task = NULL;
    solar_os_task_delete_internal(NULL);
}

static esp_err_t meshcore_ble_start(solar_os_context_t *ctx, int argc, char **argv)
{
    uint8_t bda[6];
    uint8_t addr_type = 0U;
    bool pin_supplied = false;
    uint32_t pin = 0U;
    if (!parse_args(argc, argv, bda, &addr_type, &pin_supplied, &pin)) {
        return ESP_ERR_INVALID_ARG;
    }
    char owner[SOLAR_OS_JOB_OWNER_MAX];
    esp_err_t error = solar_os_jobs_owner_name(
        solar_os_meshcore_ble_job.name, owner, sizeof(owner));
    if (error == ESP_OK) {
        error = solar_os_meshcore_ble_start(
            bda, addr_type, pin_supplied, pin, owner);
    }
    pin = 0U;
    if (error != ESP_OK) return error;

    memset(&meshcore_ble_job, 0, sizeof(meshcore_ble_job));
    if (solar_os_task_create_pinned_internal(
            meshcore_ble_task, "meshcore_ble",
            SOLAR_OS_MESHCORE_BLE_WORKER_STACK, NULL,
            tskIDLE_PRIORITY + 2, &meshcore_ble_job.task,
            tskNO_AFFINITY, SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        solar_os_meshcore_ble_stop();
        return ESP_ERR_NO_MEM;
    }
    char address[18];
    snprintf(address, sizeof(address), "%02x:%02x:%02x:%02x:%02x:%02x",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    (void)solar_os_jobs_note_resource(
        solar_os_meshcore_ble_job.name, SOLAR_OS_JOB_RESOURCE_CUSTOM,
        "meshcore-provider", "BLE companion");
    (void)solar_os_jobs_note_resource(
        solar_os_meshcore_ble_job.name, SOLAR_OS_JOB_RESOURCE_CUSTOM,
        address, "BLE peer");
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io != NULL) {
        solar_os_shell_io_printf(
            io, "meshcore-ble started: %s %s%s\n", address,
            addr_type_name(addr_type),
            pin_supplied ? " pairing" : "");
    }
    return ESP_OK;
}

static void meshcore_ble_stop(solar_os_context_t *ctx)
{
    meshcore_ble_job.stop_requested = true;
    solar_os_meshcore_ble_cancel();
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(MESHCORE_BLE_STOP_WAIT_MS);
    while (!meshcore_ble_job.task_done && meshcore_ble_job.task != NULL &&
           (int32_t)(deadline - xTaskGetTickCount()) > 0) {
        vTaskDelay(1);
    }
    if (meshcore_ble_job.task != NULL) {
        solar_os_task_delete_internal(meshcore_ble_job.task);
        meshcore_ble_job.task = NULL;
    }
    solar_os_meshcore_ble_stop();
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io != NULL) solar_os_shell_io_writeln(io, "meshcore-ble stopped");
    SOLAR_OS_LOGI(TAG, "stopped");
}

static void meshcore_ble_detail(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL) return;
    solar_os_meshcore_ble_status_t status;
    if (solar_os_meshcore_ble_get_status(&status) != ESP_OK) return;
    char address[18];
    snprintf(address, sizeof(address), "%02x:%02x:%02x:%02x:%02x:%02x",
             status.bda[0], status.bda[1], status.bda[2],
             status.bda[3], status.bda[4], status.bda[5]);
    solar_os_shell_io_printf(
        io, "  peer: %s state=%s connected=%s encrypted=%s bonded=%s mtu=%u\n",
        address, solar_os_meshcore_ble_state_name(status.state),
        status.connected ? "yes" : "no", status.encrypted ? "yes" : "no",
        status.bonded ? "yes" : "no", (unsigned)status.mtu);
    solar_os_shell_io_printf(
        io, "  device: node=%s model=%s version=%s build=%s\n",
        status.device[0] != '\0' ? status.device : "unknown",
        status.model[0] != '\0' ? status.model : "unknown",
        status.version[0] != '\0' ? status.version : "unknown",
        status.build[0] != '\0' ? status.build : "unknown");
    solar_os_shell_io_printf(
        io, "  sync: protocol=%u firmware_code=%u contacts=%u channels=%u\n",
        (unsigned)status.protocol_version, (unsigned)status.firmware_code,
        (unsigned)status.contacts,
        (unsigned)status.channels);
    solar_os_shell_io_printf(
        io, "  traffic: rx=%lu tx=%lu reconnects=%lu errors=%lu stack_min=%lu detail=%s\n",
        (unsigned long)status.received, (unsigned long)status.transmitted,
        (unsigned long)status.reconnects, (unsigned long)status.errors,
        (unsigned long)status.stack_watermark_bytes, status.detail);
}

const solar_os_job_t solar_os_meshcore_ble_job = {
    .name = "meshcore-ble",
    .summary = "MeshCore companion over BLE",
    .kind = SOLAR_OS_JOB_KIND_BACKGROUND,
    .start = meshcore_ble_start,
    .stop = meshcore_ble_stop,
    .detail = meshcore_ble_detail,
    .worker_stack_bytes = SOLAR_OS_MESHCORE_BLE_WORKER_STACK,
    .worker_stack_external = false,
};
