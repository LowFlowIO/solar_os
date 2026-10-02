#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_ble.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SOLAR_OS_MESHCORE_BLE_WORKER_STACK 8192U

typedef enum {
    SOLAR_OS_MESHCORE_BLE_STOPPED = 0,
    SOLAR_OS_MESHCORE_BLE_CONNECTING,
    SOLAR_OS_MESHCORE_BLE_PAIRING,
    SOLAR_OS_MESHCORE_BLE_SYNCING,
    SOLAR_OS_MESHCORE_BLE_ONLINE,
    SOLAR_OS_MESHCORE_BLE_BACKOFF,
} solar_os_meshcore_ble_state_t;

typedef struct {
    bool running;
    bool connected;
    bool encrypted;
    bool bonded;
    bool pin_supplied;
    uint8_t bda[6];
    uint8_t addr_type;
    uint16_t mtu;
    uint8_t protocol_version;
    uint8_t firmware_code;
    size_t contacts;
    size_t contacts_seen;
    size_t contacts_skipped;
    uint16_t companion_contact_capacity;
    size_t channels;
    uint32_t received;
    uint32_t transmitted;
    uint32_t reconnects;
    uint32_t errors;
    uint32_t stack_watermark_bytes;
    esp_err_t last_error;
    solar_os_meshcore_ble_state_t state;
    char device[41];
    char model[41];
    char version[21];
    char build[13];
    char detail[80];
} solar_os_meshcore_ble_status_t;

esp_err_t solar_os_meshcore_ble_start(const uint8_t bda[6], uint8_t addr_type,
                                      bool pin_supplied, uint32_t pin,
                                      const char *owner);
void solar_os_meshcore_ble_loop_once(void);
void solar_os_meshcore_ble_cancel(void);
void solar_os_meshcore_ble_stop(void);
esp_err_t solar_os_meshcore_ble_get_status(solar_os_meshcore_ble_status_t *status);
void solar_os_meshcore_ble_note_stack_watermark(uint32_t bytes);
const char *solar_os_meshcore_ble_state_name(solar_os_meshcore_ble_state_t state);

#ifdef __cplusplus
}
#endif
