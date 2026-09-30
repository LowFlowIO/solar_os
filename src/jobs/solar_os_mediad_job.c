#include "solar_os_mediad_job.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "solar_os_camera.h"
#include "solar_os_jobs.h"
#include "solar_os_log.h"
#include "solar_os_media.h"
#include "solar_os_rtp.h"
#include "solar_os_rtp_jpeg.h"
#include "solar_os_rtsp.h"
#include "solar_os_shell_io.h"
#include "solar_os_task.h"

#define MEDIAD_OWNER "job:mediad"
#define MEDIAD_DEFAULT_PORT 554U
#define MEDIAD_DEFAULT_FPS 5U
#define MEDIAD_MAX_FPS 30U
#define MEDIAD_JPEG_QUALITY 12U
#define MEDIAD_TASK_STACK 8192U
#define MEDIAD_TASK_PRIORITY (tskIDLE_PRIORITY + 2U)
#define MEDIAD_STOP_WAIT_MS 3000U
#define MEDIAD_SELECT_MS 100U
#define MEDIAD_RTSP_BUFFER_BYTES 1024U
#define MEDIAD_RTCP_INTERVAL_US 5000000LL
#define MEDIAD_UDP_PORT_FIRST 50000U
#define MEDIAD_UDP_PORT_LAST 50198U

typedef struct {
    bool running;
    volatile bool stop_requested;
    volatile bool worker_done;
    TaskHandle_t worker_task;
    int listen_fd;
    int client_fd;
    int rtp_fd;
    int rtcp_fd;
    uint16_t port;
    uint8_t fps;
    solar_os_camera_config_t camera_config;
    solar_os_camera_owner_t camera_owner;
    uint32_t clients;
    uint32_t rejected_clients;
    uint32_t frames;
    uint32_t dropped_frames;
    uint32_t rtp_packets;
    uint32_t rtcp_reports;
    uint64_t rtp_octets;
    uint32_t capture_errors;
    uint32_t jpeg_errors;
    uint32_t send_errors;
    esp_err_t last_error;
} mediad_state_t;

typedef struct {
    int client_fd;
    int rtp_fd;
    int rtcp_fd;
    struct sockaddr_in peer;
    struct sockaddr_in rtp_target;
    struct sockaddr_in rtcp_target;
    uint16_t server_rtp_port;
    uint16_t server_rtcp_port;
    char session[SOLAR_OS_RTSP_SESSION_MAX];
    char local_ip[INET_ADDRSTRLEN];
    char cname[48];
    bool setup;
    bool playing;
    bool close_requested;
    bool clock_ready;
    uint8_t input[MEDIAD_RTSP_BUFFER_BYTES];
    size_t input_len;
    solar_os_rtp_sender_t sender;
    solar_os_media_clock_t clock;
    uint32_t packet_count;
    uint32_t octet_count;
    int64_t next_frame_us;
    int64_t next_rtcp_us;
} mediad_session_t;

typedef struct {
    mediad_session_t *session;
} mediad_packet_context_t;

static const char *TAG = "mediad";
static mediad_state_t mediad = {
    .listen_fd = -1,
    .client_fd = -1,
    .rtp_fd = -1,
    .rtcp_fd = -1,
};
static portMUX_TYPE mediad_lock = portMUX_INITIALIZER_UNLOCKED;

static bool mediad_should_stop(void)
{
    return mediad.stop_requested;
}

static bool mediad_parse_u16(const char *text, uint16_t *value)
{
    if (text == NULL || text[0] == '\0' || value == NULL) {
        return false;
    }
    char *end = NULL;
    errno = 0;
    const unsigned long parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed == 0U ||
        parsed > UINT16_MAX) {
        return false;
    }
    *value = (uint16_t)parsed;
    return true;
}

static bool mediad_parse_start(int argc,
                               char **argv,
                               solar_os_camera_config_t *config,
                               uint8_t *fps,
                               uint16_t *port)
{
    *config = solar_os_camera_default_config();
    config->jpeg_quality = MEDIAD_JPEG_QUALITY;
    *fps = MEDIAD_DEFAULT_FPS;
    *port = MEDIAD_DEFAULT_PORT;
    bool size_set = false;
    bool fps_set = false;
    bool port_set = false;
    const int first = argc > 0 && argv != NULL && argv[0] != NULL &&
        strcmp(argv[0], "mediad") == 0 ? 1 : 0;
    for (int i = first; i < argc; i++) {
        if (argv[i] == NULL) {
            return false;
        }
        if (strcmp(argv[i], "qvga") == 0 || strcmp(argv[i], "vga") == 0) {
            if (size_set) {
                return false;
            }
            config->frame_size = strcmp(argv[i], "vga") == 0 ?
                SOLAR_OS_CAMERA_FRAME_SIZE_VGA :
                SOLAR_OS_CAMERA_FRAME_SIZE_QVGA;
            size_set = true;
        } else if (strncmp(argv[i], "port=", 5U) == 0) {
            if (port_set || !mediad_parse_u16(argv[i] + 5U, port)) {
                return false;
            }
            port_set = true;
        } else {
            uint16_t parsed = 0U;
            if (fps_set || !mediad_parse_u16(argv[i], &parsed) ||
                parsed > MEDIAD_MAX_FPS) {
                return false;
            }
            *fps = (uint8_t)parsed;
            fps_set = true;
        }
    }
    return true;
}

static void mediad_close_fd(int *fd)
{
    if (*fd >= 0) {
        (void)shutdown(*fd, SHUT_RDWR);
        (void)close(*fd);
        *fd = -1;
    }
}

static esp_err_t mediad_send_all(int fd, const char *data, size_t length)
{
    size_t offset = 0U;
    while (offset < length && !mediad_should_stop()) {
        const ssize_t written = send(fd, data + offset, length - offset, 0);
        if (written > 0) {
            offset += (size_t)written;
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else {
            return ESP_FAIL;
        }
    }
    return offset == length ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t mediad_send_response(int fd,
                                      uint32_t cseq,
                                      int status,
                                      const char *reason,
                                      const char *headers,
                                      const char *body)
{
    const size_t body_len = body != NULL ? strlen(body) : 0U;
    char response[1200];
    int written = 0;
    if (body != NULL) {
        written = snprintf(
            response,
            sizeof(response),
            "RTSP/1.0 %d %s\r\nCSeq: %" PRIu32
            "\r\nServer: SolarOS-Media/1.0\r\n%s"
            "Content-Type: application/sdp\r\nContent-Length: %u\r\n\r\n%s",
            status,
            reason,
            cseq,
            headers != NULL ? headers : "",
            (unsigned)body_len,
            body);
    } else {
        written = snprintf(
            response,
            sizeof(response),
            "RTSP/1.0 %d %s\r\nCSeq: %" PRIu32
            "\r\nServer: SolarOS-Media/1.0\r\n%s\r\n",
            status,
            reason,
            cseq,
            headers != NULL ? headers : "");
    }
    if (written < 0 || (size_t)written >= sizeof(response)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return mediad_send_all(fd, response, (size_t)written);
}

static esp_err_t mediad_open_listener(uint16_t port, int *listen_fd)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return ESP_FAIL;
    }
    const int reuse = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    const struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (const struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(fd, 1) != 0) {
        mediad_close_fd(&fd);
        return ESP_FAIL;
    }
    *listen_fd = fd;
    return ESP_OK;
}

static esp_err_t mediad_open_udp_pair(mediad_session_t *session)
{
    for (uint16_t port = MEDIAD_UDP_PORT_FIRST;
         port <= MEDIAD_UDP_PORT_LAST;
         port = (uint16_t)(port + 2U)) {
        int rtp_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        int rtcp_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (rtp_fd < 0 || rtcp_fd < 0) {
            mediad_close_fd(&rtp_fd);
            mediad_close_fd(&rtcp_fd);
            return ESP_FAIL;
        }
        const struct sockaddr_in rtp_address = {
            .sin_family = AF_INET,
            .sin_port = htons(port),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };
        const struct sockaddr_in rtcp_address = {
            .sin_family = AF_INET,
            .sin_port = htons((uint16_t)(port + 1U)),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };
        if (bind(rtp_fd,
                 (const struct sockaddr *)&rtp_address,
                 sizeof(rtp_address)) == 0 &&
            bind(rtcp_fd,
                 (const struct sockaddr *)&rtcp_address,
                 sizeof(rtcp_address)) == 0) {
            session->rtp_fd = rtp_fd;
            session->rtcp_fd = rtcp_fd;
            session->server_rtp_port = port;
            session->server_rtcp_port = (uint16_t)(port + 1U);
            portENTER_CRITICAL(&mediad_lock);
            mediad.rtp_fd = rtp_fd;
            mediad.rtcp_fd = rtcp_fd;
            portEXIT_CRITICAL(&mediad_lock);
            return ESP_OK;
        }
        mediad_close_fd(&rtp_fd);
        mediad_close_fd(&rtcp_fd);
    }
    return ESP_ERR_NOT_FOUND;
}

static void mediad_close_udp(mediad_session_t *session)
{
    mediad_close_fd(&session->rtp_fd);
    mediad_close_fd(&session->rtcp_fd);
    portENTER_CRITICAL(&mediad_lock);
    mediad.rtp_fd = -1;
    mediad.rtcp_fd = -1;
    portEXIT_CRITICAL(&mediad_lock);
    session->setup = false;
    session->playing = false;
}

static bool mediad_session_matches(const mediad_session_t *session,
                                   const solar_os_rtsp_request_t *request)
{
    return request->session[0] == '\0' ||
        strcmp(request->session, session->session) == 0;
}

static esp_err_t mediad_describe(mediad_session_t *session,
                                 const solar_os_rtsp_request_t *request)
{
    char sdp[512];
    const int sdp_len = snprintf(
        sdp,
        sizeof(sdp),
        "v=0\r\n"
        "o=- %08" PRIx32 " 1 IN IP4 %s\r\n"
        "s=SolarOS Media\r\n"
        "c=IN IP4 %s\r\n"
        "t=0 0\r\n"
        "a=control:*\r\n"
        "m=video 0 RTP/AVP 96\r\n"
        "a=rtpmap:96 JPEG/90000\r\n"
        "a=framerate:%u\r\n"
        "a=control:trackID=0\r\n",
        session->sender.ssrc,
        session->local_ip,
        session->local_ip,
        (unsigned)mediad.fps);
    if (sdp_len < 0 || (size_t)sdp_len >= sizeof(sdp)) {
        return ESP_ERR_INVALID_SIZE;
    }
    char headers[256];
    const int header_len = snprintf(
        headers,
        sizeof(headers),
        "Content-Base: rtsp://%s:%u/media/\r\n",
        session->local_ip,
        (unsigned)mediad.port);
    if (header_len < 0 || (size_t)header_len >= sizeof(headers)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return mediad_send_response(session->client_fd,
                                request->cseq,
                                200,
                                "OK",
                                headers,
                                sdp);
}

static esp_err_t mediad_setup(mediad_session_t *session,
                              const solar_os_rtsp_request_t *request)
{
    if (strstr(request->uri, "trackID=0") == NULL ||
        request->client_rtp_port == 0U || !mediad_session_matches(session, request)) {
        return mediad_send_response(session->client_fd,
                                    request->cseq,
                                    454,
                                    "Session Not Found",
                                    NULL,
                                    NULL);
    }
    mediad_close_udp(session);
    esp_err_t error = mediad_open_udp_pair(session);
    if (error != ESP_OK) {
        return mediad_send_response(session->client_fd,
                                    request->cseq,
                                    500,
                                    "Internal Server Error",
                                    NULL,
                                    NULL);
    }
    session->rtp_target = session->peer;
    session->rtp_target.sin_port = htons(request->client_rtp_port);
    session->rtcp_target = session->peer;
    session->rtcp_target.sin_port = htons(request->client_rtcp_port);
    session->setup = true;
    char headers[320];
    const int written = snprintf(
        headers,
        sizeof(headers),
        "Session: %s;timeout=60\r\n"
        "Transport: RTP/AVP;unicast;client_port=%u-%u;server_port=%u-%u;ssrc=%08" PRIX32 "\r\n",
        session->session,
        (unsigned)request->client_rtp_port,
        (unsigned)request->client_rtcp_port,
        (unsigned)session->server_rtp_port,
        (unsigned)session->server_rtcp_port,
        session->sender.ssrc);
    if (written < 0 || (size_t)written >= sizeof(headers)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return mediad_send_response(
        session->client_fd, request->cseq, 200, "OK", headers, NULL);
}

static esp_err_t mediad_play(mediad_session_t *session,
                             const solar_os_rtsp_request_t *request)
{
    if (!session->setup || !mediad_session_matches(session, request)) {
        return mediad_send_response(session->client_fd,
                                    request->cseq,
                                    454,
                                    "Session Not Found",
                                    NULL,
                                    NULL);
    }
    char headers[320];
    const int written = snprintf(
        headers,
        sizeof(headers),
        "Session: %s;timeout=60\r\n"
        "RTP-Info: url=rtsp://%s:%u/media/trackID=0;seq=%u;rtptime=%" PRIu32 "\r\n",
        session->session,
        session->local_ip,
        (unsigned)mediad.port,
        (unsigned)session->sender.sequence,
        session->sender.timestamp);
    if (written < 0 || (size_t)written >= sizeof(headers)) {
        return ESP_ERR_INVALID_SIZE;
    }
    const esp_err_t error = mediad_send_response(
        session->client_fd, request->cseq, 200, "OK", headers, NULL);
    if (error == ESP_OK) {
        session->playing = true;
        session->next_frame_us = esp_timer_get_time();
        /* A single camera buffer can predate PLAY by minutes. Use the
         * session start as the common RTP/RTCP origin, never that buffer. */
        (void)solar_os_media_clock_init(&session->clock,
                                        (uint64_t)session->next_frame_us,
                                        session->sender.timestamp,
                                        90000U);
        session->clock_ready = true;
        session->next_rtcp_us = session->next_frame_us + MEDIAD_RTCP_INTERVAL_US;
    }
    return error;
}

static esp_err_t mediad_handle_request(
    mediad_session_t *session,
    const solar_os_rtsp_request_t *request)
{
    switch (request->method) {
    case SOLAR_OS_RTSP_METHOD_OPTIONS:
        return mediad_send_response(
            session->client_fd,
            request->cseq,
            200,
            "OK",
            "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER\r\n",
            NULL);
    case SOLAR_OS_RTSP_METHOD_DESCRIBE:
        return mediad_describe(session, request);
    case SOLAR_OS_RTSP_METHOD_SETUP:
        return mediad_setup(session, request);
    case SOLAR_OS_RTSP_METHOD_PLAY:
        return mediad_play(session, request);
    case SOLAR_OS_RTSP_METHOD_GET_PARAMETER: {
        char header[64];
        const int written = snprintf(header,
                                     sizeof(header),
                                     "Session: %s;timeout=60\r\n",
                                     session->session);
        return written > 0 && (size_t)written < sizeof(header) ?
            mediad_send_response(
                session->client_fd, request->cseq, 200, "OK", header, NULL) :
            ESP_ERR_INVALID_SIZE;
    }
    case SOLAR_OS_RTSP_METHOD_TEARDOWN:
        if (!mediad_session_matches(session, request)) {
            return mediad_send_response(session->client_fd,
                                        request->cseq,
                                        454,
                                        "Session Not Found",
                                        NULL,
                                        NULL);
        }
        session->close_requested = true;
        return mediad_send_response(
            session->client_fd, request->cseq, 200, "OK", NULL, NULL);
    case SOLAR_OS_RTSP_METHOD_UNSUPPORTED:
    default:
        return mediad_send_response(session->client_fd,
                                    request->cseq,
                                    405,
                                    "Method Not Allowed",
                                    "Allow: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER\r\n",
                                    NULL);
    }
}

static esp_err_t mediad_read_requests(mediad_session_t *session)
{
    if (session->input_len >= sizeof(session->input)) {
        return ESP_ERR_INVALID_SIZE;
    }
    const ssize_t received = recv(session->client_fd,
                                  &session->input[session->input_len],
                                  sizeof(session->input) - session->input_len,
                                  0);
    if (received <= 0) {
        return ESP_FAIL;
    }
    session->input_len += (size_t)received;
    for (;;) {
        const size_t header_len = solar_os_rtsp_header_length(
            session->input, session->input_len);
        if (header_len == 0U) {
            return session->input_len < sizeof(session->input) ?
                ESP_OK : ESP_ERR_INVALID_SIZE;
        }
        solar_os_rtsp_request_t request;
        esp_err_t error = solar_os_rtsp_parse_request(
            session->input, header_len, &request);
        if (error == ESP_ERR_NOT_SUPPORTED) {
            error = mediad_send_response(session->client_fd,
                                         request.cseq,
                                         461,
                                         "Unsupported Transport",
                                         NULL,
                                         NULL);
        } else if (error != ESP_OK) {
            error = mediad_send_response(session->client_fd,
                                         request.cseq,
                                         400,
                                         "Bad Request",
                                         NULL,
                                         NULL);
        } else {
            error = mediad_handle_request(session, &request);
        }
        const size_t remaining = session->input_len - header_len;
        memmove(session->input, &session->input[header_len], remaining);
        session->input_len = remaining;
        if (error != ESP_OK || session->close_requested || remaining == 0U) {
            return error;
        }
    }
}

static esp_err_t mediad_send_rtp_packet(const uint8_t *packet,
                                        size_t packet_len,
                                        void *user)
{
    mediad_packet_context_t *context = user;
    mediad_session_t *session = context->session;
    const ssize_t sent = sendto(session->rtp_fd,
                                packet,
                                packet_len,
                                0,
                                (const struct sockaddr *)&session->rtp_target,
                                sizeof(session->rtp_target));
    if (sent != (ssize_t)packet_len) {
        return ESP_FAIL;
    }
    session->packet_count++;
    session->octet_count += (uint32_t)(packet_len - SOLAR_OS_RTP_HEADER_BYTES);
    portENTER_CRITICAL(&mediad_lock);
    mediad.rtp_packets++;
    mediad.rtp_octets += packet_len - SOLAR_OS_RTP_HEADER_BYTES;
    portEXIT_CRITICAL(&mediad_lock);
    return ESP_OK;
}

static esp_err_t mediad_send_frame(mediad_session_t *session)
{
    const solar_os_camera_frame_t *frame = NULL;
    esp_err_t error = solar_os_camera_capture(&mediad.camera_owner, &frame);
    if (error != ESP_OK) {
        portENTER_CRITICAL(&mediad_lock);
        mediad.capture_errors++;
        mediad.last_error = error;
        portEXIT_CRITICAL(&mediad_lock);
        return error;
    }
    if (frame->timestamp_us < session->clock.origin_us) {
        /* Returning the stale frame permits the single-buffer driver to
         * acquire a fresh one. Do not put a pre-session image on the wire. */
        error = solar_os_camera_release_frame(&mediad.camera_owner, frame);
        portENTER_CRITICAL(&mediad_lock);
        mediad.dropped_frames++;
        portEXIT_CRITICAL(&mediad_lock);
        return error;
    }
    solar_os_rtp_jpeg_view_t jpeg;
    error = solar_os_rtp_jpeg_parse(frame->data, frame->length, &jpeg);
    if (error == ESP_OK) {
        error = solar_os_media_clock_map(
            &session->clock, frame->timestamp_us, &session->sender.timestamp);
    }
    uint8_t packet[SOLAR_OS_MEDIA_RTP_PACKET_MAX];
    mediad_packet_context_t context = {.session = session};
    if (error == ESP_OK) {
        error = solar_os_rtp_jpeg_packetize(&session->sender,
                                             &jpeg,
                                             packet,
                                             sizeof(packet),
                                             mediad_send_rtp_packet,
                                             &context);
    }
    const esp_err_t release_error =
        solar_os_camera_release_frame(&mediad.camera_owner, frame);
    if (error == ESP_OK && release_error != ESP_OK) {
        error = release_error;
    }
    portENTER_CRITICAL(&mediad_lock);
    if (error == ESP_OK) {
        mediad.frames++;
    } else if (error == ESP_ERR_NOT_SUPPORTED ||
               error == ESP_ERR_INVALID_RESPONSE) {
        mediad.jpeg_errors++;
        mediad.last_error = error;
    } else {
        mediad.send_errors++;
        mediad.last_error = error;
    }
    portEXIT_CRITICAL(&mediad_lock);
    return error;
}

static void mediad_send_rtcp(mediad_session_t *session)
{
    uint8_t packet[128];
    size_t packet_len = 0U;
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    struct timeval wall_time = {0};
    (void)gettimeofday(&wall_time, NULL);
    const uint32_t ntp_seconds =
        (uint32_t)wall_time.tv_sec + UINT32_C(2208988800);
    const uint32_t ntp_fraction =
        (uint32_t)(((uint64_t)wall_time.tv_usec << 32U) / 1000000U);
    uint32_t rtp_timestamp = session->sender.timestamp;
    if (session->clock_ready) {
        (void)solar_os_media_clock_map(&session->clock,
                                       now_us,
                                       &rtp_timestamp);
    }
    esp_err_t error = solar_os_rtcp_sender_report(session->sender.ssrc,
                                                   ntp_seconds,
                                                   ntp_fraction,
                                                   rtp_timestamp,
                                                   session->packet_count,
                                                   session->octet_count,
                                                   session->cname,
                                                   packet,
                                                   sizeof(packet),
                                                   &packet_len);
    if (error == ESP_OK &&
        sendto(session->rtcp_fd,
               packet,
               packet_len,
               0,
               (const struct sockaddr *)&session->rtcp_target,
               sizeof(session->rtcp_target)) == (ssize_t)packet_len) {
        portENTER_CRITICAL(&mediad_lock);
        mediad.rtcp_reports++;
        portEXIT_CRITICAL(&mediad_lock);
    }
}

static void mediad_send_bye(mediad_session_t *session)
{
    if (session->rtcp_fd < 0 || !session->setup) {
        return;
    }
    uint8_t packet[8];
    size_t packet_len = 0U;
    if (solar_os_rtcp_bye(session->sender.ssrc,
                          packet,
                          sizeof(packet),
                          &packet_len) == ESP_OK) {
        (void)sendto(session->rtcp_fd,
                     packet,
                     packet_len,
                     0,
                     (const struct sockaddr *)&session->rtcp_target,
                     sizeof(session->rtcp_target));
    }
}

static void mediad_reject_pending_client(int listen_fd)
{
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    const int fd = accept(listen_fd, (struct sockaddr *)&peer, &peer_len);
    if (fd >= 0) {
        static const char response[] =
            "RTSP/1.0 453 Not Enough Bandwidth\r\nCSeq: 0\r\n"
            "Server: SolarOS-Media/1.0\r\n\r\n";
        (void)send(fd, response, sizeof(response) - 1U, 0);
        (void)close(fd);
        portENTER_CRITICAL(&mediad_lock);
        mediad.rejected_clients++;
        portEXIT_CRITICAL(&mediad_lock);
    }
}

static void mediad_serve_client(int client_fd,
                                const struct sockaddr_in *peer,
                                int listen_fd)
{
    mediad_session_t session = {
        .client_fd = client_fd,
        .rtp_fd = -1,
        .rtcp_fd = -1,
        .peer = *peer,
        .sender = {
            .payload_type = 96U,
            .sequence = (uint16_t)esp_random(),
            .timestamp = esp_random(),
            .ssrc = esp_random(),
            .max_packet_bytes = SOLAR_OS_MEDIA_RTP_PACKET_MAX,
        },
    };
    snprintf(session.session, sizeof(session.session), "%08" PRIx32, esp_random());
    struct sockaddr_in local;
    socklen_t local_len = sizeof(local);
    if (getsockname(client_fd, (struct sockaddr *)&local, &local_len) != 0 ||
        inet_ntop(AF_INET,
                  &local.sin_addr,
                  session.local_ip,
                  sizeof(session.local_ip)) == NULL) {
        strcpy(session.local_ip, "0.0.0.0");
    }
    snprintf(session.cname,
             sizeof(session.cname),
             "solaros-%08" PRIx32 "@%s",
             session.sender.ssrc,
             session.local_ip);
    portENTER_CRITICAL(&mediad_lock);
    mediad.client_fd = client_fd;
    mediad.clients++;
    portEXIT_CRITICAL(&mediad_lock);

    const int64_t frame_period_us = 1000000LL / mediad.fps;
    while (!mediad_should_stop() && !session.close_requested) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(client_fd, &read_fds);
        FD_SET(listen_fd, &read_fds);
        int max_fd = client_fd > listen_fd ? client_fd : listen_fd;
        if (session.rtcp_fd >= 0) {
            FD_SET(session.rtcp_fd, &read_fds);
            if (session.rtcp_fd > max_fd) {
                max_fd = session.rtcp_fd;
            }
        }
        struct timeval timeout = {
            .tv_sec = 0,
            .tv_usec = MEDIAD_SELECT_MS * 1000U,
        };
        const int selected = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);
        if (selected < 0 && errno != EINTR) {
            break;
        }
        if (selected > 0 && FD_ISSET(listen_fd, &read_fds)) {
            mediad_reject_pending_client(listen_fd);
        }
        if (selected > 0 && FD_ISSET(client_fd, &read_fds) &&
            mediad_read_requests(&session) != ESP_OK) {
            break;
        }
        if (session.rtcp_fd >= 0 && selected > 0 &&
            FD_ISSET(session.rtcp_fd, &read_fds)) {
            uint8_t report[256];
            (void)recv(session.rtcp_fd, report, sizeof(report), 0);
        }
        const int64_t now_us = esp_timer_get_time();
        if (session.playing && now_us >= session.next_frame_us) {
            const esp_err_t error = mediad_send_frame(&session);
            if (error != ESP_OK && error != ESP_ERR_NOT_SUPPORTED &&
                error != ESP_ERR_INVALID_RESPONSE) {
                break;
            }
            session.next_frame_us += frame_period_us;
            if (session.next_frame_us < now_us) {
                portENTER_CRITICAL(&mediad_lock);
                mediad.dropped_frames++;
                portEXIT_CRITICAL(&mediad_lock);
                session.next_frame_us = now_us + frame_period_us;
            }
        }
        if (session.playing && session.setup && now_us >= session.next_rtcp_us) {
            mediad_send_rtcp(&session);
            session.next_rtcp_us = now_us + MEDIAD_RTCP_INTERVAL_US;
        }
    }
    mediad_send_bye(&session);
    mediad_close_udp(&session);
    portENTER_CRITICAL(&mediad_lock);
    mediad.client_fd = -1;
    portEXIT_CRITICAL(&mediad_lock);
    (void)shutdown(client_fd, SHUT_RDWR);
    (void)close(client_fd);
}

static void mediad_worker(void *arg)
{
    (void)arg;
    while (!mediad_should_stop()) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(mediad.listen_fd, &read_fds);
        struct timeval timeout = {
            .tv_sec = 0,
            .tv_usec = MEDIAD_SELECT_MS * 1000U,
        };
        const int selected = select(
            mediad.listen_fd + 1, &read_fds, NULL, NULL, &timeout);
        if (selected < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (!mediad_should_stop()) {
                vTaskDelay(pdMS_TO_TICKS(MEDIAD_SELECT_MS));
            }
            continue;
        }
        if (selected == 0 || !FD_ISSET(mediad.listen_fd, &read_fds)) {
            continue;
        }
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        const int client_fd = accept(
            mediad.listen_fd, (struct sockaddr *)&peer, &peer_len);
        if (client_fd < 0) {
            continue;
        }
        struct timeval send_timeout = {.tv_sec = 1, .tv_usec = 0};
        (void)setsockopt(client_fd,
                         SOL_SOCKET,
                         SO_SNDTIMEO,
                         &send_timeout,
                         sizeof(send_timeout));
        mediad_serve_client(client_fd, &peer, mediad.listen_fd);
    }
    mediad.worker_done = true;
    solar_os_task_delete_internal(NULL);
}

static esp_err_t mediad_release_camera(void)
{
    if (mediad.camera_owner.generation == 0U) {
        return ESP_OK;
    }
    esp_err_t error = solar_os_camera_stop(&mediad.camera_owner);
    if (error == ESP_OK) {
        error = solar_os_camera_release_owner(&mediad.camera_owner);
    }
    return error;
}

static esp_err_t mediad_job_start(solar_os_context_t *ctx,
                                  int argc,
                                  char **argv)
{
    solar_os_camera_config_t config;
    uint8_t fps = 0U;
    uint16_t port = 0U;
    if (!mediad_parse_start(argc, argv, &config, &fps, &port)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (mediad.running || mediad.worker_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&mediad, 0, sizeof(mediad));
    mediad.listen_fd = -1;
    mediad.client_fd = -1;
    mediad.rtp_fd = -1;
    mediad.rtcp_fd = -1;
    mediad.port = port;
    mediad.fps = fps;
    mediad.camera_config = config;
    mediad.last_error = ESP_OK;

    esp_err_t error = solar_os_camera_acquire(MEDIAD_OWNER, &mediad.camera_owner);
    if (error == ESP_OK) {
        error = solar_os_camera_start(&mediad.camera_owner, &config);
    }
    if (error == ESP_OK) {
        error = mediad_open_listener(port, &mediad.listen_fd);
    }
    if (error != ESP_OK) {
        mediad_close_fd(&mediad.listen_fd);
        (void)mediad_release_camera();
        return error;
    }
    mediad.running = true;
    if (solar_os_task_create_pinned_internal(mediad_worker,
                                             "mediad",
                                             MEDIAD_TASK_STACK,
                                             NULL,
                                             MEDIAD_TASK_PRIORITY,
                                             &mediad.worker_task,
                                             tskNO_AFFINITY,
                                             SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        mediad.running = false;
        mediad_close_fd(&mediad.listen_fd);
        (void)mediad_release_camera();
        return ESP_ERR_NO_MEM;
    }
    (void)solar_os_jobs_note_resource(solar_os_mediad_job.name,
                                      SOLAR_OS_JOB_RESOURCE_CUSTOM,
                                      "camera",
                                      "exclusive lease");
    char resource[SOLAR_OS_JOB_RESOURCE_NAME_MAX];
    snprintf(resource, sizeof(resource), "tcp:%u", (unsigned)port);
    (void)solar_os_jobs_note_resource(solar_os_mediad_job.name,
                                      SOLAR_OS_JOB_RESOURCE_NET,
                                      resource,
                                      "RTSP listen");
    solar_os_shell_io_t *io = ctx != NULL ? solar_os_context_shell_io(ctx) : NULL;
    if (io != NULL) {
        solar_os_shell_io_printf(io,
                                 "mediad: rtsp://<device>:%u/media\n"
                                 "mediad: WARNING: unauthenticated media stream\n",
                                 (unsigned)port);
    }
    SOLAR_OS_LOGI(TAG,
                  "started: %s JPEG quality=%u fps=%u port=%u",
                  solar_os_camera_frame_size_name(config.frame_size),
                  (unsigned)config.jpeg_quality,
                  (unsigned)fps,
                  (unsigned)port);
    return ESP_OK;
}

static void mediad_job_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    if (!mediad.running && mediad.worker_task == NULL) {
        return;
    }
    mediad.stop_requested = true;
    mediad_close_fd(&mediad.listen_fd);
    portENTER_CRITICAL(&mediad_lock);
    const int client_fd = mediad.client_fd;
    const int rtp_fd = mediad.rtp_fd;
    const int rtcp_fd = mediad.rtcp_fd;
    portEXIT_CRITICAL(&mediad_lock);
    if (client_fd >= 0) {
        (void)shutdown(client_fd, SHUT_RDWR);
    }
    if (rtp_fd >= 0) {
        (void)shutdown(rtp_fd, SHUT_RDWR);
    }
    if (rtcp_fd >= 0) {
        (void)shutdown(rtcp_fd, SHUT_RDWR);
    }
    if (!solar_os_task_wait_done(mediad.worker_task,
                                 &mediad.worker_done,
                                 MEDIAD_STOP_WAIT_MS)) {
        return;
    }
    mediad.worker_task = NULL;
    mediad.worker_done = false;
    mediad.running = false;
    const esp_err_t error = mediad_release_camera();
    if (error != ESP_OK) {
        mediad.last_error = error;
    }
}

static bool mediad_job_event(solar_os_context_t *ctx,
                             const solar_os_event_t *event)
{
    (void)ctx;
    if (event != NULL && event->type == SOLAR_OS_EVENT_TICK &&
        mediad.worker_task != NULL && mediad.worker_done) {
        mediad.worker_task = NULL;
        mediad.worker_done = false;
    }
    return false;
}

static void mediad_job_detail(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = ctx != NULL ? solar_os_context_shell_io(ctx) : NULL;
    if (io == NULL) {
        return;
    }
    portENTER_CRITICAL(&mediad_lock);
    const bool connected = mediad.client_fd >= 0;
    const uint32_t clients = mediad.clients;
    const uint32_t rejected = mediad.rejected_clients;
    const uint32_t frames = mediad.frames;
    const uint32_t dropped = mediad.dropped_frames;
    const uint32_t packets = mediad.rtp_packets;
    const uint32_t reports = mediad.rtcp_reports;
    const uint64_t octets = mediad.rtp_octets;
    const uint32_t capture_errors = mediad.capture_errors;
    const uint32_t jpeg_errors = mediad.jpeg_errors;
    const uint32_t send_errors = mediad.send_errors;
    const esp_err_t last_error = mediad.last_error;
    portEXIT_CRITICAL(&mediad_lock);
    solar_os_shell_io_printf(
        io,
        "  RTSP: port=%u client=%s sessions=%" PRIu32 " rejected=%" PRIu32 "\n"
        "  video: %s fps=%u frames=%" PRIu32 " dropped=%" PRIu32
        " RTP=%" PRIu32 " octets=%" PRIu64 " RTCP=%" PRIu32 "\n"
        "  errors: capture=%" PRIu32 " jpeg=%" PRIu32 " send=%" PRIu32
        " last=%s\n",
        (unsigned)mediad.port,
        connected ? "connected" : "none",
        clients,
        rejected,
        solar_os_camera_frame_size_name(mediad.camera_config.frame_size),
        (unsigned)mediad.fps,
        frames,
        dropped,
        packets,
        octets,
        reports,
        capture_errors,
        jpeg_errors,
        send_errors,
        esp_err_to_name(last_error));
}

const solar_os_job_t solar_os_mediad_job = {
    .name = "mediad",
    .summary = "RTSP/RTP media publisher",
    .kind = SOLAR_OS_JOB_KIND_BACKGROUND,
    .start = mediad_job_start,
    .stop = mediad_job_stop,
    .event = mediad_job_event,
    .worker_stack_bytes = MEDIAD_TASK_STACK,
    .worker_stack_external = false,
    .detail = mediad_job_detail,
};
