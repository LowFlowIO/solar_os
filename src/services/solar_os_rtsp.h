#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SOLAR_OS_RTSP_URI_MAX 192U
#define SOLAR_OS_RTSP_SESSION_MAX 24U

typedef enum {
    SOLAR_OS_RTSP_METHOD_OPTIONS = 0,
    SOLAR_OS_RTSP_METHOD_DESCRIBE,
    SOLAR_OS_RTSP_METHOD_SETUP,
    SOLAR_OS_RTSP_METHOD_PLAY,
    SOLAR_OS_RTSP_METHOD_TEARDOWN,
    SOLAR_OS_RTSP_METHOD_GET_PARAMETER,
    SOLAR_OS_RTSP_METHOD_UNSUPPORTED,
} solar_os_rtsp_method_t;

typedef struct {
    solar_os_rtsp_method_t method;
    char uri[SOLAR_OS_RTSP_URI_MAX];
    uint32_t cseq;
    char session[SOLAR_OS_RTSP_SESSION_MAX];
    uint16_t client_rtp_port;
    uint16_t client_rtcp_port;
} solar_os_rtsp_request_t;

size_t solar_os_rtsp_header_length(const uint8_t *data, size_t length);
esp_err_t solar_os_rtsp_parse_request(const uint8_t *data,
                                      size_t length,
                                      solar_os_rtsp_request_t *request);
const char *solar_os_rtsp_method_name(solar_os_rtsp_method_t method);
