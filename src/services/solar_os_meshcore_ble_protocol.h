#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SOLAR_OS_MESHCORE_BLE_FRAME_MAX 176U
#define SOLAR_OS_MESHCORE_BLE_PUBLIC_KEY_SIZE 32U
#define SOLAR_OS_MESHCORE_BLE_PUBLIC_KEY_PREFIX_SIZE 6U
#define SOLAR_OS_MESHCORE_BLE_CHANNEL_NAME_MAX 32U
#define SOLAR_OS_MESHCORE_BLE_CONTACT_NAME_MAX 32U
#define SOLAR_OS_MESHCORE_BLE_TEXT_MAX 133U
#define SOLAR_OS_MESHCORE_BLE_CHANNEL_CAPACITY 8U
#define SOLAR_OS_MESHCORE_BLE_PROTOCOL_VERSION 3U
#define SOLAR_OS_MESHCORE_BLE_BUILD_MAX 12U
#define SOLAR_OS_MESHCORE_BLE_MODEL_MAX 40U
#define SOLAR_OS_MESHCORE_BLE_VERSION_MAX 20U

typedef enum {
    SOLAR_OS_MESHCORE_BLE_RESP_OK = 0x00,
    SOLAR_OS_MESHCORE_BLE_RESP_ERROR = 0x01,
    SOLAR_OS_MESHCORE_BLE_RESP_CONTACTS_START = 0x02,
    SOLAR_OS_MESHCORE_BLE_RESP_CONTACT = 0x03,
    SOLAR_OS_MESHCORE_BLE_RESP_CONTACTS_END = 0x04,
    SOLAR_OS_MESHCORE_BLE_RESP_SELF_INFO = 0x05,
    SOLAR_OS_MESHCORE_BLE_RESP_SENT = 0x06,
    SOLAR_OS_MESHCORE_BLE_RESP_CONTACT_MESSAGE = 0x07,
    SOLAR_OS_MESHCORE_BLE_RESP_CHANNEL_MESSAGE = 0x08,
    SOLAR_OS_MESHCORE_BLE_RESP_NO_MORE_MESSAGES = 0x0a,
    SOLAR_OS_MESHCORE_BLE_RESP_DEVICE_INFO = 0x0d,
    SOLAR_OS_MESHCORE_BLE_RESP_CONTACT_MESSAGE_V3 = 0x10,
    SOLAR_OS_MESHCORE_BLE_RESP_CHANNEL_MESSAGE_V3 = 0x11,
    SOLAR_OS_MESHCORE_BLE_RESP_CHANNEL_INFO = 0x12,
    SOLAR_OS_MESHCORE_BLE_PUSH_ADVERT = 0x80,
    SOLAR_OS_MESHCORE_BLE_PUSH_PATH_UPDATED = 0x81,
    SOLAR_OS_MESHCORE_BLE_PUSH_SEND_CONFIRMED = 0x82,
    SOLAR_OS_MESHCORE_BLE_PUSH_MESSAGES_WAITING = 0x83,
    SOLAR_OS_MESHCORE_BLE_PUSH_NEW_ADVERT = 0x8a,
} solar_os_meshcore_ble_frame_type_t;

typedef struct {
    uint8_t public_key[SOLAR_OS_MESHCORE_BLE_PUBLIC_KEY_SIZE];
    uint8_t type;
    uint8_t flags;
    int8_t out_path_len;
    char name[SOLAR_OS_MESHCORE_BLE_CONTACT_NAME_MAX + 1U];
    uint32_t last_advert;
    int32_t latitude;
    int32_t longitude;
    uint32_t last_modified;
} solar_os_meshcore_ble_contact_t;

typedef struct {
    uint8_t index;
    char name[SOLAR_OS_MESHCORE_BLE_CHANNEL_NAME_MAX + 1U];
    uint8_t secret[16];
} solar_os_meshcore_ble_channel_t;

typedef struct {
    uint8_t public_key_prefix[SOLAR_OS_MESHCORE_BLE_PUBLIC_KEY_PREFIX_SIZE];
    uint8_t path_len;
    uint8_t text_type;
    uint32_t timestamp;
    bool has_snr;
    int8_t snr_quarters;
    char text[SOLAR_OS_MESHCORE_BLE_TEXT_MAX + 1U];
} solar_os_meshcore_ble_contact_message_t;

typedef struct {
    uint8_t channel_index;
    uint8_t path_len;
    uint8_t text_type;
    uint32_t timestamp;
    bool has_snr;
    int8_t snr_quarters;
    char text[SOLAR_OS_MESHCORE_BLE_TEXT_MAX + 1U];
} solar_os_meshcore_ble_channel_message_t;

typedef struct {
    uint8_t firmware_code;
    uint16_t max_contacts;
    uint8_t max_channels;
    char build[SOLAR_OS_MESHCORE_BLE_BUILD_MAX + 1U];
    char model[SOLAR_OS_MESHCORE_BLE_MODEL_MAX + 1U];
    char version[SOLAR_OS_MESHCORE_BLE_VERSION_MAX + 1U];
} solar_os_meshcore_ble_device_info_t;

size_t solar_os_meshcore_ble_build_app_start(uint8_t *frame, size_t capacity,
                                             const char *name);
size_t solar_os_meshcore_ble_build_device_query(uint8_t *frame, size_t capacity);
size_t solar_os_meshcore_ble_build_set_time(uint8_t *frame, size_t capacity,
                                            uint32_t epoch_seconds);
size_t solar_os_meshcore_ble_build_get_contacts(uint8_t *frame, size_t capacity);
size_t solar_os_meshcore_ble_build_get_channel(uint8_t *frame, size_t capacity,
                                               uint8_t channel_index);
size_t solar_os_meshcore_ble_build_sync_next(uint8_t *frame, size_t capacity);
size_t solar_os_meshcore_ble_build_send_direct(
    uint8_t *frame, size_t capacity, const uint8_t public_key[32],
    uint32_t timestamp, const char *text);
size_t solar_os_meshcore_ble_build_send_channel(
    uint8_t *frame, size_t capacity, uint8_t channel_index,
    uint32_t timestamp, const char *text);

bool solar_os_meshcore_ble_parse_contact(
    const uint8_t *frame, size_t length, solar_os_meshcore_ble_contact_t *contact);
bool solar_os_meshcore_ble_parse_channel(
    const uint8_t *frame, size_t length, solar_os_meshcore_ble_channel_t *channel);
bool solar_os_meshcore_ble_parse_device_info(
    const uint8_t *frame, size_t length,
    solar_os_meshcore_ble_device_info_t *device);
bool solar_os_meshcore_ble_parse_contact_message(
    const uint8_t *frame, size_t length,
    solar_os_meshcore_ble_contact_message_t *message);
bool solar_os_meshcore_ble_parse_channel_message(
    const uint8_t *frame, size_t length,
    solar_os_meshcore_ble_channel_message_t *message);
uint64_t solar_os_meshcore_ble_message_key(const uint8_t *frame, size_t length);
