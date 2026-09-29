#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_meshcore_ble_protocol.h"

static void put_u32(uint8_t *value, uint32_t number)
{
    value[0] = (uint8_t)number;
    value[1] = (uint8_t)(number >> 8U);
    value[2] = (uint8_t)(number >> 16U);
    value[3] = (uint8_t)(number >> 24U);
}

static void test_builders(void)
{
    uint8_t frame[SOLAR_OS_MESHCORE_BLE_FRAME_MAX] = {0};
    size_t length = solar_os_meshcore_ble_build_app_start(
        frame, sizeof(frame), "SolarOS");
    assert(length == 15U);
    assert(frame[0] == 0x01U && frame[1] == 0x01U);
    assert(memcmp(frame + 8U, "SolarOS", 7U) == 0);

    assert(solar_os_meshcore_ble_build_device_query(frame, sizeof(frame)) == 2U);
    assert(frame[0] == 0x16U && frame[1] == 0x03U);
    assert(solar_os_meshcore_ble_build_set_time(
               frame, sizeof(frame), UINT32_C(0x12345678)) == 5U);
    const uint8_t set_time[] = {0x06U, 0x78U, 0x56U, 0x34U, 0x12U};
    assert(memcmp(frame, set_time, sizeof(set_time)) == 0);

    uint8_t public_key[SOLAR_OS_MESHCORE_BLE_PUBLIC_KEY_SIZE];
    for (size_t i = 0; i < sizeof(public_key); i++) public_key[i] = (uint8_t)i;
    length = solar_os_meshcore_ble_build_send_direct(
        frame, sizeof(frame), public_key, UINT32_C(0x12345678), "hello");
    assert(length == 18U);
    const uint8_t direct_prefix[] = {
        0x02U, 0x00U, 0x00U, 0x78U, 0x56U, 0x34U, 0x12U,
        0x00U, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U,
    };
    assert(memcmp(frame, direct_prefix, sizeof(direct_prefix)) == 0);
    assert(memcmp(frame + sizeof(direct_prefix), "hello", 5U) == 0);

    length = solar_os_meshcore_ble_build_send_channel(
        frame, sizeof(frame), 3U, UINT32_C(0x12345678), "hello");
    const uint8_t channel[] = {
        0x03U, 0x00U, 0x03U, 0x78U, 0x56U, 0x34U, 0x12U,
        'h', 'e', 'l', 'l', 'o',
    };
    assert(length == sizeof(channel));
    assert(memcmp(frame, channel, sizeof(channel)) == 0);

    char too_long[SOLAR_OS_MESHCORE_BLE_TEXT_MAX + 2U];
    memset(too_long, 'x', sizeof(too_long) - 1U);
    too_long[sizeof(too_long) - 1U] = '\0';
    assert(solar_os_meshcore_ble_build_send_direct(
               frame, sizeof(frame), public_key, 0U, too_long) == 0U);
    assert(solar_os_meshcore_ble_build_get_channel(
               frame, sizeof(frame), SOLAR_OS_MESHCORE_BLE_CHANNEL_CAPACITY) == 0U);
}

static void test_contacts_and_channels(void)
{
    uint8_t frame[148] = {0};
    frame[0] = SOLAR_OS_MESHCORE_BLE_RESP_CONTACT;
    for (size_t i = 0; i < 32U; i++) frame[1U + i] = (uint8_t)(0xa0U + i);
    frame[33] = 1U;
    frame[34] = 2U;
    frame[35] = 0xffU;
    memcpy(frame + 100U, "Alice", 5U);
    put_u32(frame + 132U, 11U);
    put_u32(frame + 136U, (uint32_t)-123);
    put_u32(frame + 140U, 456U);
    put_u32(frame + 144U, 22U);
    solar_os_meshcore_ble_contact_t contact;
    assert(solar_os_meshcore_ble_parse_contact(frame, sizeof(frame), &contact));
    assert(contact.public_key[0] == 0xa0U && contact.public_key[31] == 0xbfU);
    assert(contact.type == 1U && contact.flags == 2U && contact.out_path_len == -1);
    assert(strcmp(contact.name, "Alice") == 0);
    assert(contact.last_advert == 11U && contact.latitude == -123);
    assert(contact.longitude == 456 && contact.last_modified == 22U);
    assert(!solar_os_meshcore_ble_parse_contact(frame, sizeof(frame) - 1U, &contact));

    uint8_t channel_frame[50] = {0};
    channel_frame[0] = SOLAR_OS_MESHCORE_BLE_RESP_CHANNEL_INFO;
    channel_frame[1] = 2U;
    memcpy(channel_frame + 2U, "Field", 5U);
    memset(channel_frame + 34U, 0x5a, 16U);
    solar_os_meshcore_ble_channel_t channel;
    assert(solar_os_meshcore_ble_parse_channel(
        channel_frame, sizeof(channel_frame), &channel));
    assert(channel.index == 2U && strcmp(channel.name, "Field") == 0);
    assert(channel.secret[0] == 0x5aU && channel.secret[15] == 0x5aU);

    uint8_t device_frame[82] = {0};
    device_frame[0] = SOLAR_OS_MESHCORE_BLE_RESP_DEVICE_INFO;
    device_frame[1] = 13U;
    device_frame[2] = 32U;
    device_frame[3] = 8U;
    memcpy(device_frame + 8U, "20260929", 8U);
    memcpy(device_frame + 20U, "Heltec V3", 9U);
    memcpy(device_frame + 60U, "v1.17.1", 7U);
    solar_os_meshcore_ble_device_info_t device;
    assert(solar_os_meshcore_ble_parse_device_info(
        device_frame, sizeof(device_frame), &device));
    assert(device.firmware_code == 13U && device.max_contacts == 64U);
    assert(device.max_channels == 8U && strcmp(device.build, "20260929") == 0);
    assert(strcmp(device.model, "Heltec V3") == 0);
    assert(strcmp(device.version, "v1.17.1") == 0);
    assert(!solar_os_meshcore_ble_parse_device_info(device_frame, 79U, &device));
}

static void test_messages(void)
{
    const uint8_t direct[] = {
        0x07U, 1U, 2U, 3U, 4U, 5U, 6U, 0xffU, 0U,
        0x78U, 0x56U, 0x34U, 0x12U, 'h', 'i',
    };
    solar_os_meshcore_ble_contact_message_t contact;
    assert(solar_os_meshcore_ble_parse_contact_message(
        direct, sizeof(direct), &contact));
    assert(contact.public_key_prefix[5] == 6U && contact.path_len == 0xffU);
    assert(!contact.has_snr && contact.timestamp == UINT32_C(0x12345678));
    assert(strcmp(contact.text, "hi") == 0);

    const uint8_t signed_direct[] = {
        0x10U, (uint8_t)-8, 0U, 0U,
        1U, 2U, 3U, 4U, 5U, 6U, 1U, 2U,
        0x78U, 0x56U, 0x34U, 0x12U,
        0xdeU, 0xadU, 0xbeU, 0xefU, 's', 'i', 'g',
    };
    assert(solar_os_meshcore_ble_parse_contact_message(
        signed_direct, sizeof(signed_direct), &contact));
    assert(contact.has_snr && contact.snr_quarters == -8);
    assert(contact.text_type == 2U && strcmp(contact.text, "sig") == 0);
    assert(!solar_os_meshcore_ble_parse_contact_message(
        signed_direct, 19U, &contact));

    const uint8_t channel[] = {
        0x11U, 12U, 0U, 0U, 3U, 0xffU, 0U,
        0x04U, 0x03U, 0x02U, 0x01U, 'B', 'o', 'b', ':', ' ', 'h', 'i',
    };
    solar_os_meshcore_ble_channel_message_t group;
    assert(solar_os_meshcore_ble_parse_channel_message(
        channel, sizeof(channel), &group));
    assert(group.has_snr && group.snr_quarters == 12 && group.channel_index == 3U);
    assert(group.timestamp == UINT32_C(0x01020304));
    assert(strcmp(group.text, "Bob: hi") == 0);

    const uint64_t key = solar_os_meshcore_ble_message_key(direct, sizeof(direct));
    assert(key != 0U);
    assert(key == solar_os_meshcore_ble_message_key(direct, sizeof(direct)));
    assert(key != solar_os_meshcore_ble_message_key(channel, sizeof(channel)));
}

int main(void)
{
    test_builders();
    test_contacts_and_channels();
    test_messages();
    puts("meshcore BLE protocol tests: ok");
    return 0;
}
