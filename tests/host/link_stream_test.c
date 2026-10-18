#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_link_stream.c"

struct test_queue { size_t size, used, head; uint8_t bytes[]; };
static unsigned queue_count;
static uint32_t now_ms, random_value = 100;
static solar_os_port_driver_t ports[2];
static bool online[2], legacy_peer;
static unsigned lose_open, lose_ack, lose_data;
static bool duplicate_data;
static bool reopen_during_send;
typedef struct { unsigned destination; solar_os_link_message_t message; } packet_t;
static packet_t packets[64];
static size_t packet_head, packet_count;

size_t strlcpy(char *dst, const char *src, size_t size) {
    size_t n = strlen(src);
    if (size) { size_t c = n < size ? n : size - 1; memcpy(dst, src, c); dst[c] = 0; }
    return n;
}
uint32_t esp_random(void) { return ++random_value; }
TickType_t xTaskGetTickCount(void) { return now_ms; }
void vTaskDelay(TickType_t ticks) { now_ms += ticks; }
QueueHandle_t solar_os_queue_create(UBaseType_t length, UBaseType_t size) {
    assert(size == 1);
    QueueHandle_t q = calloc(1, sizeof(*q) + length);
    assert(q); q->size = length; queue_count++; return q;
}
void solar_os_queue_delete(QueueHandle_t q) { if (q) { assert(queue_count); queue_count--; free(q); } }
void xQueueReset(QueueHandle_t q) { q->head = q->used = 0; }
unsigned uxQueueMessagesWaiting(QueueHandle_t q) { return q->used; }
unsigned uxQueueSpacesAvailable(QueueHandle_t q) { return q->size - q->used; }
int xQueueSend(QueueHandle_t q, const void *p, unsigned ticks) {
    if (q->used == q->size) { now_ms += ticks; return pdFALSE; }
    q->bytes[(q->head + q->used++) % q->size] = *(const uint8_t *)p; return pdTRUE;
}
int xQueueReceive(QueueHandle_t q, void *p, unsigned ticks) {
    if (!q->used) { now_ms += ticks; return pdFALSE; }
    *(uint8_t *)p = q->bytes[q->head++ % q->size]; q->used--; return pdTRUE;
}
esp_err_t solar_os_port_register(const solar_os_port_driver_t *driver) {
    unsigned i = driver->name[0] == 'a' ? 0 : 1;
    assert(!ports[i].name); ports[i] = *driver; return ESP_OK;
}
esp_err_t solar_os_port_unregister(const char *name) {
    unsigned i = name[0] == 'a' ? 0 : 1;
    if (((link_stream_t *)ports[i].user)->port_open) return ESP_ERR_INVALID_STATE;
    memset(&ports[i], 0, sizeof(ports[i])); return ESP_OK;
}
esp_err_t solar_os_link_get_status(const char *name, solar_os_link_status_t *status) {
    *status = (solar_os_link_status_t){.local_id = name[0] == 'a' ? 1 : 2, .frame_mtu = 250};
    return ESP_OK;
}
esp_err_t solar_os_link_send(const char *name, solar_os_link_message_type_t type,
    uint32_t destination, const void *data, size_t len, uint16_t *sequence) {
    (void)sequence;
    unsigned source = name[0] == 'a' ? 0 : 1, target = 1 - source;
    if (!online[source]) return ESP_ERR_INVALID_STATE;
    const uint8_t *p = data;
    assert(type == SOLAR_OS_LINK_MESSAGE_STREAM && destination == target + 1);
    assert(len >= 16 && p[0] == 0x53 && p[1] == 0x56 && p[2] == 2);
    if (reopen_during_send && source == 0 && p[3] == LINK_STREAM_OPCODE_OPEN) {
        reopen_during_send = false;
        assert(ports[0].close(ports[0].user) == ESP_OK);
        assert(ports[0].open(ports[0].user) == ESP_OK);
    }
    if (!online[target]) return ESP_OK;
    if (p[3] == LINK_STREAM_OPCODE_OPEN && lose_open) { lose_open--; return ESP_OK; }
    if (p[3] == LINK_STREAM_OPCODE_ACK && lose_ack) { lose_ack--; return ESP_OK; }
    if (p[3] == LINK_STREAM_OPCODE_DATA && lose_data) { lose_data--; return ESP_OK; }
    unsigned copies = p[3] == LINK_STREAM_OPCODE_DATA && duplicate_data ? 2 : 1;
    for (unsigned i = 0; i < copies; i++) {
        assert(packet_count < 64);
        packet_t *packet = &packets[(packet_head + packet_count++) % 64];
        *packet = (packet_t){.destination = target, .message = {
            .source = source + 1, .destination = target + 1, .type = type, .payload_len = len}};
        memcpy(packet->message.payload, p, len);
    }
    return ESP_OK;
}
static const char *links[2] = {"a", "b"};
static const char *names[2] = {"a0", "b0"};
static void pump(unsigned duration) {
    uint32_t until = now_ms + duration;
    while ((int32_t)(until - now_ms) > 0) {
        for (unsigned i = 0; i < 2; i++) if (online[i]) solar_os_link_stream_process(links[i], now_ms);
        while (packet_count) {
            packet_t packet = packets[packet_head++ % 64]; packet_count--;
            if (!online[packet.destination]) continue;
            link_stream_t *peer = &streams[packet.destination];
            bool legacy_same_open = legacy_peer && packet.destination == 1 &&
                packet.message.payload[3] == LINK_STREAM_OPCODE_OPEN &&
                peer->remote_session == stream_read_u32(packet.message.payload + 4);
            bool was_open_sent = peer->open_sent;
            uint32_t was_next_open = peer->next_open_ms;
            /* Reproduce the previous v2 peer's unconditional transmit-epoch
             * rotation on a changed OPEN, with the identical wire format. */
            if (legacy_peer && packet.destination == 1 &&
                packet.message.payload[3] == LINK_STREAM_OPCODE_OPEN && peer->had_remote_session &&
                peer->remote_session != stream_read_u32(packet.message.payload + 4)) {
                peer->local_session_confirmed = true;
            }
            (void)solar_os_link_stream_ingest(links[packet.destination], &packet.message, now_ms);
            if (legacy_same_open) {
                /* Older peers do not answer a repeated, unchanged OPEN. */
                peer->open_sent = was_open_sent;
                peer->next_open_ms = was_next_open;
            }
        }
        now_ms++;
    }
}
static void write_bytes(unsigned i, const uint8_t *data, size_t len) {
    size_t n;
    assert(ports[i].write(ports[i].user, data, len, &n) == ESP_OK && n == len);
}
static void read_bytes(unsigned i, const uint8_t *expected, size_t len) {
    uint8_t data[2049]; size_t n;
    assert(len < sizeof(data));
    assert(ports[i].read(ports[i].user, data, sizeof(data), 0, &n) == ESP_OK);
    assert(n == len && !memcmp(data, expected, len));
    assert(ports[i].read(ports[i].user, data, sizeof(data), 0, &n) == ESP_ERR_TIMEOUT && !n);
}
static void exchange(void) {
    uint8_t a[391], b[271];
    for (size_t i = 0; i < sizeof(a); i++) a[i] = (uint8_t)(i * 17 + now_ms);
    for (size_t i = 0; i < sizeof(b); i++) b[i] = (uint8_t)(i * 29 + now_ms);
    write_bytes(0, a, sizeof(a)); write_bytes(1, b, sizeof(b)); pump(4000);
    read_bytes(1, a, sizeof(a)); read_bytes(0, b, sizeof(b));
    assert(streams[0].tx_window_count == 0 && streams[1].tx_window_count == 0);
    uint32_t a_epoch = streams[0].local_session, b_epoch = streams[1].local_session;
    pump(16000); /* Include periodic OPENs, not just the first successful byte. */
    assert(a_epoch == streams[0].local_session && b_epoch == streams[1].local_session);
}
static void stop(unsigned i) {
    online[i] = false; solar_os_link_stream_transport_stopped(links[i]);
    if (legacy_peer && i == 1) { streams[i].remote_session = 0; streams[i].remote_last_seen_ms = 0; }
    solar_os_link_stream_status_t status;
    assert(solar_os_link_stream_get_status(names[i], &status) == ESP_OK && !status.connected);
}
static void run_pair(bool legacy) {
    legacy_peer = legacy; online[0] = online[1] = true;
    for (unsigned i = 0; i < 2; i++) {
        assert(solar_os_link_stream_create(links[i], names[i], 2 - i) == ESP_OK);
        assert(ports[i].open(ports[i].user) == ESP_OK);
    }
    lose_open = 3; pump(4000); assert(!lose_open);
    exchange();
    lose_ack = 1; duplicate_data = true; exchange(); duplicate_data = false;
    assert(!lose_ack && streams[0].counters.retries + streams[1].counters.retries);
    lose_data = 2; exchange(); assert(!lose_data);
    reopen_during_send = true;
    streams[0].open_sent = false;
    solar_os_link_stream_process(links[0], now_ms);
    assert(!reopen_during_send && !streams[0].open_sent);
    pump(4000); exchange();
    for (unsigned cycle = 0; cycle < 12; cycle++) {
        unsigned i = cycle % 2;
        assert(ports[i].close(ports[i].user) == ESP_OK);
        if (cycle % 3) pump(100); /* Also cover a lost CLOSE / immediate reopen. */
        assert(ports[i].open(ports[i].user) == ESP_OK);
        pump(2500); exchange();
        const uint8_t pending[] = "queued across a transport interruption";
        stop(i); write_bytes(1 - i, pending, sizeof(pending)); pump(2000);
        online[i] = true; pump(4000); read_bytes(i, pending, sizeof(pending)); exchange();
    }
    stop(0); stop(1); pump(50000);
    online[0] = online[1] = true; pump(4000); exchange();
    /* A full queue applies backpressure and closing drops its old bytes. */
    stop(1);
    uint8_t full[2049] = {0}; size_t written = 0;
    assert(ports[0].write(ports[0].user, full, sizeof(full), &written) == ESP_ERR_TIMEOUT);
    assert(written == 2048);
    assert(solar_os_link_stream_remove(names[0]) == ESP_ERR_INVALID_STATE);
    for (unsigned i = 0; i < 2; i++) {
        assert(ports[i].close(ports[i].user) == ESP_OK);
        assert(solar_os_link_stream_remove(names[i]) == ESP_OK);
    }
    packet_count = packet_head = 0;
    assert(!queue_count && !solar_os_link_stream_count());
}
int main(void) {
    run_pair(false); run_pair(true);
    now_ms = UINT32_MAX - 50000;
    run_pair(false);
    puts("Link v2: lost OPEN/DATA/ACK, duplicate data, reopen during send, transport restart, legacy reset, timer wrap, backpressure and queue cleanup PASS");
}
