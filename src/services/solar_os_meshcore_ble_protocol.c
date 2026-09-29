#include "solar_os_meshcore_ble_protocol.h"

#include <string.h>

enum {
    CMD_APP_START = 0x01,
    CMD_SEND_DIRECT = 0x02,
    CMD_SEND_CHANNEL = 0x03,
    CMD_GET_CONTACTS = 0x04,
    CMD_SET_TIME = 0x06,
    CMD_SYNC_NEXT = 0x0a,
    CMD_DEVICE_QUERY = 0x16,
    CMD_GET_CHANNEL = 0x1f,
};

static uint32_t read_u32(const uint8_t *value)
{
    return (uint32_t)value[0] |
        ((uint32_t)value[1] << 8U) |
        ((uint32_t)value[2] << 16U) |
        ((uint32_t)value[3] << 24U);
}

static void write_u32(uint8_t *value, uint32_t number)
{
    value[0] = (uint8_t)number;
    value[1] = (uint8_t)(number >> 8U);
    value[2] = (uint8_t)(number >> 16U);
    value[3] = (uint8_t)(number >> 24U);
}

static void copy_text(char *destination, size_t capacity,
                      const uint8_t *source, size_t length)
{
    if (capacity == 0U) {
        return;
    }
    size_t copied = length < capacity - 1U ? length : capacity - 1U;
    while (copied > 0U && source[copied - 1U] == '\0') {
        copied--;
    }
    memcpy(destination, source, copied);
    destination[copied] = '\0';
}

static size_t bounded_text_length(const char *text, size_t maximum)
{
    size_t length = 0U;
    while (length <= maximum && text[length] != '\0') length++;
    return length;
}

size_t solar_os_meshcore_ble_build_app_start(uint8_t *frame, size_t capacity,
                                             const char *name)
{
    if (frame == NULL || name == NULL) {
        return 0U;
    }
    const size_t name_len = bounded_text_length(
        name, SOLAR_OS_MESHCORE_BLE_TEXT_MAX);
    if (name_len > SOLAR_OS_MESHCORE_BLE_TEXT_MAX || capacity < 8U + name_len) {
        return 0U;
    }
    memset(frame, 0, 8U + name_len);
    frame[0] = CMD_APP_START;
    frame[1] = 1U;
    memcpy(frame + 8U, name, name_len);
    return 8U + name_len;
}

size_t solar_os_meshcore_ble_build_device_query(uint8_t *frame, size_t capacity)
{
    if (frame == NULL || capacity < 2U) {
        return 0U;
    }
    frame[0] = CMD_DEVICE_QUERY;
    frame[1] = SOLAR_OS_MESHCORE_BLE_PROTOCOL_VERSION;
    return 2U;
}

size_t solar_os_meshcore_ble_build_set_time(uint8_t *frame, size_t capacity,
                                            uint32_t epoch_seconds)
{
    if (frame == NULL || capacity < 5U) {
        return 0U;
    }
    frame[0] = CMD_SET_TIME;
    write_u32(frame + 1U, epoch_seconds);
    return 5U;
}

size_t solar_os_meshcore_ble_build_get_contacts(uint8_t *frame, size_t capacity)
{
    if (frame == NULL || capacity < 1U) {
        return 0U;
    }
    frame[0] = CMD_GET_CONTACTS;
    return 1U;
}

size_t solar_os_meshcore_ble_build_get_channel(uint8_t *frame, size_t capacity,
                                               uint8_t channel_index)
{
    if (frame == NULL || capacity < 2U ||
        channel_index >= SOLAR_OS_MESHCORE_BLE_CHANNEL_CAPACITY) {
        return 0U;
    }
    frame[0] = CMD_GET_CHANNEL;
    frame[1] = channel_index;
    return 2U;
}

size_t solar_os_meshcore_ble_build_sync_next(uint8_t *frame, size_t capacity)
{
    if (frame == NULL || capacity < 1U) {
        return 0U;
    }
    frame[0] = CMD_SYNC_NEXT;
    return 1U;
}

size_t solar_os_meshcore_ble_build_send_direct(
    uint8_t *frame, size_t capacity, const uint8_t public_key[32],
    uint32_t timestamp, const char *text)
{
    if (frame == NULL || public_key == NULL || text == NULL) {
        return 0U;
    }
    const size_t text_len = bounded_text_length(
        text, SOLAR_OS_MESHCORE_BLE_TEXT_MAX);
    const size_t length = 13U + text_len;
    if (text_len > SOLAR_OS_MESHCORE_BLE_TEXT_MAX || capacity < length) {
        return 0U;
    }
    frame[0] = CMD_SEND_DIRECT;
    frame[1] = 0U;
    frame[2] = 0U;
    write_u32(frame + 3U, timestamp);
    memcpy(frame + 7U, public_key, SOLAR_OS_MESHCORE_BLE_PUBLIC_KEY_PREFIX_SIZE);
    memcpy(frame + 13U, text, text_len);
    return length;
}

size_t solar_os_meshcore_ble_build_send_channel(
    uint8_t *frame, size_t capacity, uint8_t channel_index,
    uint32_t timestamp, const char *text)
{
    if (frame == NULL || text == NULL ||
        channel_index >= SOLAR_OS_MESHCORE_BLE_CHANNEL_CAPACITY) {
        return 0U;
    }
    const size_t text_len = bounded_text_length(
        text, SOLAR_OS_MESHCORE_BLE_TEXT_MAX);
    const size_t length = 7U + text_len;
    if (text_len > SOLAR_OS_MESHCORE_BLE_TEXT_MAX || capacity < length) {
        return 0U;
    }
    frame[0] = CMD_SEND_CHANNEL;
    frame[1] = 0U;
    frame[2] = channel_index;
    write_u32(frame + 3U, timestamp);
    memcpy(frame + 7U, text, text_len);
    return length;
}

bool solar_os_meshcore_ble_parse_contact(
    const uint8_t *frame, size_t length, solar_os_meshcore_ble_contact_t *contact)
{
    if (frame == NULL || contact == NULL || length < 148U ||
        frame[0] != SOLAR_OS_MESHCORE_BLE_RESP_CONTACT) {
        return false;
    }
    memset(contact, 0, sizeof(*contact));
    memcpy(contact->public_key, frame + 1U, sizeof(contact->public_key));
    contact->type = frame[33];
    contact->flags = frame[34];
    contact->out_path_len = (int8_t)frame[35];
    copy_text(contact->name, sizeof(contact->name), frame + 100U, 32U);
    contact->last_advert = read_u32(frame + 132U);
    contact->latitude = (int32_t)read_u32(frame + 136U);
    contact->longitude = (int32_t)read_u32(frame + 140U);
    contact->last_modified = read_u32(frame + 144U);
    return true;
}

bool solar_os_meshcore_ble_parse_channel(
    const uint8_t *frame, size_t length, solar_os_meshcore_ble_channel_t *channel)
{
    if (frame == NULL || channel == NULL || length != 50U ||
        frame[0] != SOLAR_OS_MESHCORE_BLE_RESP_CHANNEL_INFO ||
        frame[1] >= SOLAR_OS_MESHCORE_BLE_CHANNEL_CAPACITY) {
        return false;
    }
    memset(channel, 0, sizeof(*channel));
    channel->index = frame[1];
    copy_text(channel->name, sizeof(channel->name), frame + 2U, 32U);
    memcpy(channel->secret, frame + 34U, sizeof(channel->secret));
    return true;
}

bool solar_os_meshcore_ble_parse_device_info(
    const uint8_t *frame, size_t length,
    solar_os_meshcore_ble_device_info_t *device)
{
    if (frame == NULL || device == NULL || length < 2U ||
        frame[0] != SOLAR_OS_MESHCORE_BLE_RESP_DEVICE_INFO) {
        return false;
    }
    memset(device, 0, sizeof(*device));
    device->firmware_code = frame[1];
    if (device->firmware_code < 3U) {
        return true;
    }
    if (length < 80U) {
        return false;
    }
    device->max_contacts = (uint16_t)frame[2] * 2U;
    device->max_channels = frame[3];
    copy_text(device->build, sizeof(device->build), frame + 8U,
              SOLAR_OS_MESHCORE_BLE_BUILD_MAX);
    copy_text(device->model, sizeof(device->model), frame + 20U,
              SOLAR_OS_MESHCORE_BLE_MODEL_MAX);
    copy_text(device->version, sizeof(device->version), frame + 60U,
              SOLAR_OS_MESHCORE_BLE_VERSION_MAX);
    return true;
}

bool solar_os_meshcore_ble_parse_contact_message(
    const uint8_t *frame, size_t length,
    solar_os_meshcore_ble_contact_message_t *message)
{
    if (frame == NULL || message == NULL || length == 0U) {
        return false;
    }
    size_t offset = 1U;
    const bool v3 = frame[0] == SOLAR_OS_MESHCORE_BLE_RESP_CONTACT_MESSAGE_V3;
    if (!v3 && frame[0] != SOLAR_OS_MESHCORE_BLE_RESP_CONTACT_MESSAGE) {
        return false;
    }
    if (v3) {
        if (length < 16U) return false;
        offset = 4U;
    } else if (length < 13U) {
        return false;
    }
    memset(message, 0, sizeof(*message));
    message->has_snr = v3;
    message->snr_quarters = v3 ? (int8_t)frame[1] : 0;
    memcpy(message->public_key_prefix, frame + offset,
           sizeof(message->public_key_prefix));
    offset += sizeof(message->public_key_prefix);
    message->path_len = frame[offset++];
    message->text_type = frame[offset++];
    message->timestamp = read_u32(frame + offset);
    offset += 4U;
    if (message->text_type == 2U) {
        if (length < offset + 4U) return false;
        offset += 4U;
    }
    copy_text(message->text, sizeof(message->text), frame + offset,
              length - offset);
    return true;
}

bool solar_os_meshcore_ble_parse_channel_message(
    const uint8_t *frame, size_t length,
    solar_os_meshcore_ble_channel_message_t *message)
{
    if (frame == NULL || message == NULL || length == 0U) {
        return false;
    }
    size_t offset = 1U;
    const bool v3 = frame[0] == SOLAR_OS_MESHCORE_BLE_RESP_CHANNEL_MESSAGE_V3;
    if (!v3 && frame[0] != SOLAR_OS_MESHCORE_BLE_RESP_CHANNEL_MESSAGE) {
        return false;
    }
    if (v3) {
        if (length < 11U) return false;
        offset = 4U;
    } else if (length < 8U) {
        return false;
    }
    memset(message, 0, sizeof(*message));
    message->has_snr = v3;
    message->snr_quarters = v3 ? (int8_t)frame[1] : 0;
    message->channel_index = frame[offset++];
    if (message->channel_index >= SOLAR_OS_MESHCORE_BLE_CHANNEL_CAPACITY) {
        return false;
    }
    message->path_len = frame[offset++];
    message->text_type = frame[offset++];
    message->timestamp = read_u32(frame + offset);
    offset += 4U;
    copy_text(message->text, sizeof(message->text), frame + offset,
              length - offset);
    return true;
}

uint64_t solar_os_meshcore_ble_message_key(const uint8_t *frame, size_t length)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    if (frame == NULL) {
        return 0U;
    }
    for (size_t i = 0; i < length; i++) {
        hash ^= frame[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash != 0U ? hash : 1U;
}
