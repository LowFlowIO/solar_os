/* Real RTSP client, response/SDP parser, RTP/JPEG assembler, L16 jitter and
 * PCM converter over loopback sockets. Only OS allocation/tasks/audio mocked. */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "../../src/services/solar_os_rtsp_client.c"

static atomic_uint allocations, allocation_calls, fail_allocation, live_tasks;
static atomic_uint samples_played, audio_opens, audio_closes, callbacks;
static atomic_bool fail_audio;
static atomic_uint random_value = 1234;

void *solar_os_memory_alloc(size_t n, solar_os_memory_class_t kind, const char *tag)
{
    (void)kind; assert(!strncmp(tag, "rtsp.", 5));
    if (atomic_fetch_add(&allocation_calls, 1) + 1 == atomic_load(&fail_allocation)) return NULL;
    void *p = malloc(n); if (p) atomic_fetch_add(&allocations, 1); return p;
}
void *solar_os_memory_calloc(size_t n, size_t size, solar_os_memory_class_t kind, const char *tag)
{
    void *p = solar_os_memory_alloc(n * size, kind, tag); if (p) memset(p, 0, n * size); return p;
}
void solar_os_memory_free(void *p) { if (p) { atomic_fetch_sub(&allocations, 1); free(p); } }
int64_t esp_timer_get_time(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
uint32_t esp_random(void) { return atomic_fetch_add(&random_value, 1); }
void vTaskDelay(TickType_t ticks)
{
    struct timespec t = {.tv_sec = ticks / 1000, .tv_nsec = ticks % 1000 * 1000000L}; nanosleep(&t, NULL);
}
void vTaskSuspend(TaskHandle_t task) { assert(!task); pthread_exit(NULL); }
struct test_task { pthread_t thread; TaskFunction_t fn; void *arg; };
static void *task_entry(void *arg) { struct test_task *t = arg; t->fn(t->arg); return NULL; }
BaseType_t solar_os_task_create_pinned_internal(TaskFunction_t fn, const char *name, uint32_t stack,
    void *arg, UBaseType_t priority, TaskHandle_t *handle, BaseType_t core, solar_os_task_role_t role)
{
    (void)priority; (void)core; assert(role == SOLAR_OS_TASK_ROLE_FOREGROUND && stack == 8192);
    assert(!strcmp(name, "rtsp-sink"));
    struct test_task *t = calloc(1, sizeof(*t)); assert(t);
    t->fn = fn; t->arg = arg; *handle = t; atomic_fetch_add(&live_tasks, 1);
    assert(!pthread_create(&t->thread, NULL, task_entry, t)); return pdPASS;
}
void solar_os_task_delete_internal(TaskHandle_t task)
{
    assert(task); pthread_join(task->thread, NULL); free(task); atomic_fetch_sub(&live_tasks, 1);
}
esp_err_t solar_os_net_resolve_host(const char *host, char *ip, size_t capacity)
{ assert(!strcmp(host, "127.0.0.1")); snprintf(ip, capacity, "%s", host); return ESP_OK; }
struct solar_os_audio_player { pthread_t owner; solar_os_audio_player_options_t options; };
esp_err_t solar_os_audio_player_create(const solar_os_audio_player_options_t *o,
    solar_os_audio_player_t **p, solar_os_stream_audio_format_t *format, solar_os_audio_device_info_t *device)
{
    (void)device;
    if (atomic_load(&fail_audio)) return ESP_ERR_NOT_FOUND;
    assert(!o->buffered && o->volume == SOLAR_OS_AUDIO_VOLUME_GLOBAL && o->open_timeout_ms == 500);
    *p = malloc(sizeof(**p)); assert(*p); (*p)->owner = pthread_self(); (*p)->options = *o;
    *format = (solar_os_stream_audio_format_t){.sample_rate = 48000, .channels = 2,
        .bits_per_sample = 16, .sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE};
    atomic_fetch_add(&audio_opens, 1); return ESP_OK;
}
esp_err_t solar_os_audio_player_write(solar_os_audio_player_t *p, const void *data,
    size_t bytes, const volatile bool *cancelled)
{
    assert(pthread_equal(p->owner, pthread_self()));
    if (*cancelled) return ESP_ERR_TIMEOUT;
    const int16_t *samples = data;
    assert(samples[0] == 0x1234 || samples[0] == 0); /* L16 network byte order. */
    vTaskDelay((uint32_t)(bytes / 4 * 1000 / 48000));
    p->options.samples(data, bytes / 2, 2, p->options.user);
    atomic_fetch_add(&samples_played, bytes / 2); return ESP_OK;
}
void solar_os_audio_player_destroy(solar_os_audio_player_t *p)
{
    if (p) { assert(pthread_equal(p->owner, pthread_self())); free(p); atomic_fetch_add(&audio_closes, 1); }
}

typedef struct {
    int listen, control;
    uint16_t port, video_port, audio_port;
    bool offer_video, offer_audio, reject, stall;
    atomic_bool stop, playing, teardown;
    unsigned setup_video, setup_audio;
    pthread_t thread;
    client_track_t video, audio;
    struct sockaddr_in video_peer, audio_peer;
} test_server_t;

static void send_response(test_server_t *s, uint32_t seq, const char *headers, const char *body)
{
    char text[2048];
    int len = snprintf(text, sizeof(text), "RTSP/1.0 %s\r\nCSeq: %lu\r\n%sContent-Length: %zu\r\n\r\n%s",
        s->reject ? "401 Unauthorized" : "200 OK", (unsigned long)seq, headers ? headers : "", body ? strlen(body) : 0, body ? body : "");
    assert(len > 0 && (size_t)len < sizeof(text));
    /* Deliberately fragmented headers and SDP exercise streaming framing. */
    for (int pos = 0; pos < len;) {
        int n = len - pos > 17 ? 17 : len - pos;
        int sent = send(s->control, text + pos, n, MSG_NOSIGNAL);
        if (sent <= 0) return;
        pos += sent;
    }
}

static esp_err_t send_jpeg_packet(const uint8_t *data, size_t len, void *user)
{
    test_server_t *s = user;
    assert(sendto(s->video.rtp, data, len, 0, (struct sockaddr *)&s->video_peer, sizeof(s->video_peer)) == (ssize_t)len);
    return ESP_OK;
}

static void *server_worker(void *arg)
{
    test_server_t *s = arg;
    s->control = accept(s->listen, NULL, NULL); assert(s->control >= 0);
    uint8_t input[2048]; size_t used = 0;
    uint64_t origin = now_us(), last_audio = 0, last_video = 0, last_rtcp = 0;
    uint16_t audio_seq = 65535;
    solar_os_rtp_sender_t video_sender = {.payload_type = 96, .sequence = 1, .ssrc = 1234, .max_packet_bytes = 1200};
    uint32_t audio_ts = 0xffffff00U;
    uint8_t packet[1200], scan[80] = {0};
    solar_os_rtp_jpeg_view_t view = {.scan = scan, .scan_len = sizeof(scan), .width = 320, .height = 240, .type = 1};
    memset(view.quant_tables, 1, sizeof(view.quant_tables));
    while (!atomic_load(&s->stop)) {
        fd_set set; FD_ZERO(&set); FD_SET(s->control, &set);
        struct timeval timeout = {.tv_usec = 1000};
        if (select(s->control + 1, &set, NULL, NULL, &timeout) > 0) {
            int n = recv(s->control, input + used, sizeof(input) - used, 0);
            if (n <= 0) break;
            used += n;
            if (solar_os_rtsp_header_length(input, used)) {
                solar_os_rtsp_request_t r;
                assert(solar_os_rtsp_parse_request(input, used, &r) == ESP_OK); used = 0;
                if (r.method == SOLAR_OS_RTSP_METHOD_TEARDOWN) { atomic_store(&s->teardown, true); break; }
                if (s->stall) continue;
                char headers[512] = "", body[1024] = "";
                if (r.method == SOLAR_OS_RTSP_METHOD_DESCRIBE) {
                    snprintf(headers, sizeof(headers), "Content-Base: rtsp://127.0.0.1:%u/media/\r\nContent-Type: application/sdp\r\n", s->port);
                    snprintf(body, sizeof(body), "v=0\r\na=control:*\r\n%s%s",
                        s->offer_video ? "m=video 0 RTP/AVP 96\r\na=rtpmap:96 JPEG/90000\r\na=control:trackID=0\r\n" : "",
                        s->offer_audio ? "m=audio 0 RTP/AVP 97\r\na=rtpmap:97 L16/16000/1\r\na=control:trackID=1\r\n" : "");
                } else if (r.method == SOLAR_OS_RTSP_METHOD_SETUP) {
                    bool video = strstr(r.uri, "trackID=0") != NULL;
                    if (video) s->setup_video++; else s->setup_audio++;
                    struct sockaddr_in *peer = video ? &s->video_peer : &s->audio_peer;
                    *peer = (struct sockaddr_in){.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK), .sin_port = htons(r.client_rtp_port)};
                    unsigned port = video ? s->video.local_port : s->audio.local_port;
                    snprintf(headers, sizeof(headers), "Session: testing;timeout=60\r\nTransport: RTP/AVP/UDP;unicast;client_port=%u-%u;server_port=%u-%u\r\n",
                        r.client_rtp_port, r.client_rtcp_port, port, port + 1);
                } else if (r.method == SOLAR_OS_RTSP_METHOD_PLAY) {
                    assert(!strcmp(r.session, "testing")); origin = now_us();
                    atomic_store(&s->playing, true);
                }
                send_response(s, r.cseq, headers, body);
                if (s->reject) break;
            }
        }
        if (!atomic_load(&s->playing)) continue;
        uint64_t elapsed = now_us() - origin;
        if (s->setup_audio && elapsed >= last_audio + 10000) {
            solar_os_rtp_header_t h = {.payload_type = 97, .sequence = audio_seq++, .timestamp = audio_ts, .ssrc = 5678};
            assert(solar_os_rtp_header_encode(&h, packet, sizeof(packet)) == ESP_OK);
            for (size_t i = 12; i < 332; i += 2) { packet[i] = 0x12; packet[i + 1] = 0x34; }
            assert(sendto(s->audio.rtp, packet, 332, 0, (struct sockaddr *)&s->audio_peer, sizeof(s->audio_peer)) == 332);
            audio_ts += 160; last_audio += 10000;
        }
        if (s->setup_video && elapsed >= last_video + 40000) {
            video_sender.timestamp = (uint32_t)(elapsed * 90000 / 1000000);
            assert(solar_os_rtp_jpeg_packetize(&video_sender, &view, packet, sizeof(packet), send_jpeg_packet, s) == ESP_OK);
            last_video = elapsed;
        }
        if (elapsed >= last_rtcp + 50000) {
            client_track_t *tracks[] = {&s->video, &s->audio};
            struct sockaddr_in *peers[] = {&s->video_peer, &s->audio_peer};
            for (unsigned i = 0; i < 2; i++) {
                if (!(i ? s->setup_audio : s->setup_video)) continue;
                uint32_t rate = i ? 16000 : 90000;
                uint32_t timestamp = (uint32_t)(elapsed * rate / 1000000) + (i ? 0xffffff00U : 0);
                size_t bytes;
                assert(solar_os_rtcp_sender_report(i ? 5678 : 1234, 100 + elapsed / 1000000,
                    (uint32_t)((elapsed % 1000000) * (1ULL << 32) / 1000000), timestamp,
                    10, 1000, "same-source", packet, sizeof(packet), &bytes) == ESP_OK);
                struct sockaddr_in to = *peers[i]; to.sin_port = htons(ntohs(to.sin_port) + 1);
                assert(sendto(tracks[i]->rtcp, packet, bytes, 0, (struct sockaddr *)&to, sizeof(to)) == (ssize_t)bytes);
            }
            last_rtcp = elapsed;
        }
    }
    close(s->control); return NULL;
}

static void server_start(test_server_t *s)
{
    s->video.rtp = s->video.rtcp = s->audio.rtp = s->audio.rtcp = -1;
    assert(open_udp(&s->video) == ESP_OK && open_udp(&s->audio) == ESP_OK);
    s->listen = socket(AF_INET, SOCK_STREAM, 0); assert(s->listen >= 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(!bind(s->listen, (struct sockaddr *)&addr, sizeof(addr)) && !listen(s->listen, 1));
    socklen_t len = sizeof(addr); assert(!getsockname(s->listen, (struct sockaddr *)&addr, &len));
    s->port = ntohs(addr.sin_port);
    assert(!pthread_create(&s->thread, NULL, server_worker, s));
}
static void server_stop(test_server_t *s)
{
    atomic_store(&s->stop, true); pthread_join(s->thread, NULL);
    close(s->listen); close_track(&s->video); close_track(&s->audio);
}
static void samples_callback(const int16_t *samples, size_t count, uint8_t channels, void *user)
{ (void)samples; (void)user; assert(count && channels == 2); atomic_fetch_add(&callbacks, 1); }
static void *run_client(void *arg) { solar_os_rtsp_client_run(arg); return NULL; }

static void test_play(bool video, bool audio, bool audio_only)
{
    unsigned opens_before = atomic_load(&audio_opens);
    test_server_t server = {.offer_video = video, .offer_audio = audio}; server_start(&server);
    char url[192]; snprintf(url, sizeof(url), "rtsp://127.0.0.1:%u/media", server.port);
    solar_os_rtsp_client_options_t options = {.video = !audio_only, .audio = true, .samples = samples_callback};
    solar_os_rtsp_client_t *c; assert(solar_os_rtsp_client_create(url, &options, &c) == ESP_OK);
    pthread_t thread; assert(!pthread_create(&thread, NULL, run_client, c));
    solar_os_rtsp_client_status_t status;
    uint64_t deadline = now_us() + 2000000;
    do { vTaskDelay(5); solar_os_rtsp_client_status(c, &status); } while (!status.playing && !c->cancel && now_us() < deadline);
    assert(status.playing && status.video == (video && !audio_only) && status.audio == audio);
    assert(solar_os_rtsp_client_destroy(c) == ESP_ERR_INVALID_STATE);
    assert(solar_os_rtsp_client_run(c) == ESP_ERR_INVALID_STATE);
    if (video && !audio_only) {
        solar_os_rtp_jpeg_frame_t frame;
        while (!solar_os_rtsp_client_take_video(c, &frame) && now_us() < deadline) vTaskDelay(1);
        assert(c->leased && frame.length > 100);
        uint8_t saved[128]; memcpy(saved, frame.data, sizeof(saved));
        vTaskDelay(120); assert(!memcmp(saved, frame.data, sizeof(saved)));
        assert(!solar_os_rtsp_client_take_video(c, &frame));
        solar_os_rtsp_client_release_video(c);
        while (!solar_os_rtsp_client_take_video(c, &frame) && now_us() < deadline) vTaskDelay(1);
        assert(c->leased); /* Keep the final frame leased across run() exit. */
    } else { assert(!c->jpeg); vTaskDelay(150); }
    if (audio) assert(atomic_load(&audio_opens) == opens_before + 1 && atomic_load(&samples_played) > 0 && atomic_load(&callbacks));
    else assert(atomic_load(&audio_opens) == opens_before);
    if (video && audio && !audio_only) {
        lock(c); assert(c->video.clock.valid && c->audio.clock.valid); unlock(c);
    }
    solar_os_rtsp_client_cancel(c); pthread_join(thread, NULL);
    if (video && !audio_only) {
        assert(solar_os_rtsp_client_destroy(c) == ESP_ERR_INVALID_STATE);
        solar_os_rtsp_client_release_video(c);
    }
    assert(solar_os_rtsp_client_destroy(c) == ESP_OK); server_stop(&server);
    assert(server.setup_video == (unsigned)(video && !audio_only) && server.setup_audio == (unsigned)audio);
    assert(atomic_load(&allocations) == 0 && atomic_load(&live_tasks) == 0);
    assert(atomic_load(&audio_opens) == atomic_load(&audio_closes));
    assert(atomic_load(&server.teardown));
}

static void test_failure(bool rejected, bool stalled, bool output_failed, unsigned allocation_failure)
{
    test_server_t s = {.offer_audio = true, .offer_video = allocation_failure != 0, .reject = rejected, .stall = stalled}; server_start(&s);
    atomic_store(&fail_audio, output_failed);
    char url[192]; snprintf(url, sizeof(url), "rtsp://127.0.0.1:%u/media", s.port);
    solar_os_rtsp_client_options_t options = {.audio = true, .video = allocation_failure != 0}; solar_os_rtsp_client_t *c;
    assert(solar_os_rtsp_client_create(url, &options, &c) == ESP_OK);
    atomic_store(&allocation_calls, 0); atomic_store(&fail_allocation, allocation_failure);
    pthread_t thread; pthread_create(&thread, NULL, run_client, c);
    if (stalled) { vTaskDelay(20); solar_os_rtsp_client_cancel(c); }
    pthread_join(thread, NULL);
    solar_os_rtsp_client_status_t status; solar_os_rtsp_client_status(c, &status);
    assert(status.error != ESP_OK);
    solar_os_rtsp_client_destroy(c); server_stop(&s);
    atomic_store(&fail_audio, false);
    atomic_store(&fail_allocation, 0);
    assert(!atomic_load(&allocations) && !atomic_load(&live_tasks));
}

int main(void)
{
    for (unsigned i = 1; i <= 3; i++) {
        atomic_store(&allocation_calls, 0); atomic_store(&fail_allocation, i);
        solar_os_rtsp_client_t *c = NULL;
        solar_os_rtsp_client_options_t o = {.audio = true};
        assert(solar_os_rtsp_client_create("rtsp://127.0.0.1/media", &o, &c) == ESP_ERR_NO_MEM && !c);
        assert(!atomic_load(&allocations));
    }
    atomic_store(&fail_allocation, 0);
    test_play(true, false, false); test_play(false, true, false);
    test_play(true, true, false); test_play(true, true, true);
    test_failure(true, false, false, 0); test_failure(false, true, false, 0); test_failure(false, false, true, 0);
    for (unsigned i = 1; i <= 3; i++) test_failure(false, false, false, i);
    puts("rtsp_client_test: OK"); return 0;
}
