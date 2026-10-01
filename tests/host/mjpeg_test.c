#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_mjpeg.h"

typedef struct {
    uint8_t frames[4][32];
    size_t lengths[4];
    size_t count;
    esp_err_t result;
} capture_t;

static esp_err_t capture_frame(const uint8_t *jpeg, size_t jpeg_len, void *user)
{
    capture_t *capture = user;
    assert(capture != NULL);
    assert(capture->count < 4U);
    assert(jpeg_len <= sizeof(capture->frames[0]));
    memcpy(capture->frames[capture->count], jpeg, jpeg_len);
    capture->lengths[capture->count] = jpeg_len;
    capture->count++;
    return capture->result;
}

static void test_extracts_chunked_frames(void)
{
    uint8_t buffer[32];
    capture_t capture = {0};
    solar_os_mjpeg_parser_t parser;
    assert(solar_os_mjpeg_parser_init(
               &parser, buffer, sizeof(buffer), capture_frame, &capture) == ESP_OK);

    static const uint8_t first[] = {
        '-', '-', 'b', '\r', '\n', 0xff,
    };
    static const uint8_t second[] = {
        0xd8, 0x01, 0xff, 0x00, 0x02, 0xff,
    };
    static const uint8_t third[] = {
        0xd9, '\r', '\n', '-', '-', 'b', '\r', '\n', 0xff, 0xd8, 0x03, 0xff, 0xd9,
    };
    assert(solar_os_mjpeg_parser_feed(&parser, first, sizeof(first)) == ESP_OK);
    assert(solar_os_mjpeg_parser_feed(&parser, second, sizeof(second)) == ESP_OK);
    assert(solar_os_mjpeg_parser_feed(&parser, third, sizeof(third)) == ESP_OK);

    static const uint8_t expected_first[] = {
        0xff, 0xd8, 0x01, 0xff, 0x00, 0x02, 0xff, 0xd9,
    };
    static const uint8_t expected_second[] = {
        0xff, 0xd8, 0x03, 0xff, 0xd9,
    };
    assert(capture.count == 2U);
    assert(capture.lengths[0] == sizeof(expected_first));
    assert(memcmp(capture.frames[0], expected_first, sizeof(expected_first)) == 0);
    assert(capture.lengths[1] == sizeof(expected_second));
    assert(memcmp(capture.frames[1], expected_second, sizeof(expected_second)) == 0);
    assert(parser.frames == 2U);
    assert(parser.dropped_frames == 0U);
}

static void test_drops_oversize_and_recovers(void)
{
    uint8_t buffer[6];
    capture_t capture = {0};
    solar_os_mjpeg_parser_t parser;
    assert(solar_os_mjpeg_parser_init(
               &parser, buffer, sizeof(buffer), capture_frame, &capture) == ESP_OK);

    static const uint8_t stream[] = {
        0xff, 0xd8, 1, 2, 3, 4, 5, 6, 0xff, 0xd9,
        0xff, 0xd8, 7, 0xff, 0xd9,
    };
    assert(solar_os_mjpeg_parser_feed(&parser, stream, sizeof(stream)) == ESP_OK);
    static const uint8_t expected[] = {0xff, 0xd8, 7, 0xff, 0xd9};
    assert(capture.count == 1U);
    assert(capture.lengths[0] == sizeof(expected));
    assert(memcmp(capture.frames[0], expected, sizeof(expected)) == 0);
    assert(parser.frames == 1U);
    assert(parser.dropped_frames == 1U);
}

static void test_reset_discards_partial_frame(void)
{
    uint8_t buffer[16];
    capture_t capture = {0};
    solar_os_mjpeg_parser_t parser;
    assert(solar_os_mjpeg_parser_init(
               &parser, buffer, sizeof(buffer), capture_frame, &capture) == ESP_OK);

    static const uint8_t partial[] = {0xff, 0xd8, 1, 2};
    static const uint8_t complete[] = {0xff, 0xd8, 3, 0xff, 0xd9};
    assert(solar_os_mjpeg_parser_feed(&parser, partial, sizeof(partial)) == ESP_OK);
    solar_os_mjpeg_parser_reset(&parser);
    assert(solar_os_mjpeg_parser_feed(&parser, complete, sizeof(complete)) == ESP_OK);
    assert(capture.count == 1U);
    assert(capture.frames[0][2] == 3U);
}

static void test_propagates_callback_error_after_complete_frame(void)
{
    uint8_t buffer[16];
    capture_t capture = {.result = ESP_FAIL};
    solar_os_mjpeg_parser_t parser;
    assert(solar_os_mjpeg_parser_init(
               &parser, buffer, sizeof(buffer), capture_frame, &capture) == ESP_OK);

    static const uint8_t frame[] = {0xff, 0xd8, 1, 0xff, 0xd9};
    assert(solar_os_mjpeg_parser_feed(&parser, frame, sizeof(frame)) == ESP_FAIL);
    assert(capture.count == 1U);
    assert(parser.frames == 1U);
    assert(!parser.collecting);
}

int main(void)
{
    test_extracts_chunked_frames();
    test_drops_oversize_and_recovers();
    test_reset_discards_partial_frame();
    test_propagates_callback_error_after_complete_frame();
    puts("MJPEG parser tests: ok");
    return 0;
}
