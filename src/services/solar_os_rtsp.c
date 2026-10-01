#include "solar_os_rtsp.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

size_t solar_os_rtsp_header_length(const uint8_t *data, size_t length)
{
    if (data == NULL) {
        return 0U;
    }
    for (size_t i = 0U; i + 3U < length; i++) {
        if (data[i] == '\r' && data[i + 1U] == '\n' &&
            data[i + 2U] == '\r' && data[i + 3U] == '\n') {
            return i + 4U;
        }
    }
    return 0U;
}

static solar_os_rtsp_method_t rtsp_parse_method(const char *text,
                                                 size_t length)
{
    static const struct {
        const char *name;
        solar_os_rtsp_method_t method;
    } methods[] = {
        {"OPTIONS", SOLAR_OS_RTSP_METHOD_OPTIONS},
        {"DESCRIBE", SOLAR_OS_RTSP_METHOD_DESCRIBE},
        {"SETUP", SOLAR_OS_RTSP_METHOD_SETUP},
        {"PLAY", SOLAR_OS_RTSP_METHOD_PLAY},
        {"TEARDOWN", SOLAR_OS_RTSP_METHOD_TEARDOWN},
        {"GET_PARAMETER", SOLAR_OS_RTSP_METHOD_GET_PARAMETER},
    };
    for (size_t i = 0U; i < sizeof(methods) / sizeof(methods[0]); i++) {
        if (strlen(methods[i].name) == length &&
            memcmp(text, methods[i].name, length) == 0) {
            return methods[i].method;
        }
    }
    return SOLAR_OS_RTSP_METHOD_UNSUPPORTED;
}

static bool rtsp_parse_u32(const char *text, uint32_t *value)
{
    if (text == NULL || value == NULL || !isdigit((unsigned char)*text)) {
        return false;
    }
    char *end = NULL;
    errno = 0;
    const unsigned long parsed = strtoul(text, &end, 10);
    while (end != NULL && (*end == ' ' || *end == '\t')) {
        end++;
    }
    if (errno != 0 || end == text || end == NULL || *end != '\0' ||
        parsed > UINT32_MAX) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static bool rtsp_parse_client_ports(const char *value,
                                    uint16_t *rtp,
                                    uint16_t *rtcp)
{
    const char *field = value;
    while (field != NULL && *field != '\0') {
        while (*field == ' ' || *field == '\t' || *field == ';') {
            field++;
        }
        if (strncasecmp(field, "client_port=", 12U) == 0) {
            char *end = NULL;
            errno = 0;
            const unsigned long first = strtoul(field + 12U, &end, 10);
            if (errno != 0 || end == field + 12U || *end != '-') {
                return false;
            }
            const unsigned long second = strtoul(end + 1U, &end, 10);
            if (errno != 0 || first == 0U || first > UINT16_MAX ||
                second != first + 1U || second > UINT16_MAX ||
                (*end != '\0' && *end != ';' && *end != ' ' && *end != '\t')) {
                return false;
            }
            *rtp = (uint16_t)first;
            *rtcp = (uint16_t)second;
            return true;
        }
        field = strchr(field, ';');
    }
    return false;
}

static char *rtsp_trim(char *text)
{
    while (*text == ' ' || *text == '\t') {
        text++;
    }
    char *end = text + strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t')) {
        *--end = '\0';
    }
    return text;
}

esp_err_t solar_os_rtsp_parse_request(const uint8_t *data,
                                      size_t length,
                                      solar_os_rtsp_request_t *request)
{
    const size_t header_length = solar_os_rtsp_header_length(data, length);
    if (data == NULL || request == NULL || header_length == 0U ||
        header_length >= 1024U) {
        return ESP_ERR_INVALID_ARG;
    }
    char text[1024];
    memcpy(text, data, header_length);
    text[header_length] = '\0';
    memset(request, 0, sizeof(*request));

    char *line_end = strstr(text, "\r\n");
    if (line_end == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *line_end = '\0';
    char *first_space = strchr(text, ' ');
    char *second_space = first_space != NULL ? strchr(first_space + 1U, ' ') : NULL;
    if (first_space == NULL || second_space == NULL ||
        strcmp(second_space + 1U, "RTSP/1.0") != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    request->method = rtsp_parse_method(text, (size_t)(first_space - text));
    const size_t uri_length = (size_t)(second_space - first_space - 1U);
    if (uri_length == 0U || uri_length >= sizeof(request->uri)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(request->uri, first_space + 1U, uri_length);
    request->uri[uri_length] = '\0';

    bool cseq_seen = false;
    esp_err_t header_error = ESP_OK;
    char *line = line_end + 2U;
    while (line[0] != '\0' && !(line[0] == '\r' && line[1] == '\n')) {
        line_end = strstr(line, "\r\n");
        if (line_end == NULL) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        *line_end = '\0';
        char *colon = strchr(line, ':');
        if (colon == NULL) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        *colon = '\0';
        char *value = rtsp_trim(colon + 1U);
        if (strcasecmp(line, "CSeq") == 0) {
            if (cseq_seen || !rtsp_parse_u32(value, &request->cseq)) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            cseq_seen = true;
        } else if (strcasecmp(line, "Session") == 0) {
            char *separator = strchr(value, ';');
            if (separator != NULL) {
                *separator = '\0';
            }
            value = rtsp_trim(value);
            if (value[0] == '\0' ||
                strlen(value) >= sizeof(request->session)) {
                return ESP_ERR_INVALID_SIZE;
            }
            strcpy(request->session, value);
        } else if (strcasecmp(line, "Transport") == 0) {
            /* UDP is the default lower transport; clients may name it
             * explicitly. TCP interleaving remains unsupported. */
            if ((strncasecmp(value, "RTP/AVP;", 8U) != 0 &&
                 strncasecmp(value, "RTP/AVP/UDP;", 12U) != 0) ||
                strstr(value, "unicast") == NULL ||
                !rtsp_parse_client_ports(value,
                                         &request->client_rtp_port,
                                         &request->client_rtcp_port)) {
                header_error = ESP_ERR_NOT_SUPPORTED;
            }
        } else if (strcasecmp(line, "Content-Length") == 0) {
            uint32_t content_length = 0U;
            if (!rtsp_parse_u32(value, &content_length) || content_length != 0U) {
                header_error = ESP_ERR_NOT_SUPPORTED;
            }
        }
        line = line_end + 2U;
    }
    return cseq_seen ? header_error : ESP_ERR_INVALID_RESPONSE;
}

const char *solar_os_rtsp_method_name(solar_os_rtsp_method_t method)
{
    switch (method) {
    case SOLAR_OS_RTSP_METHOD_OPTIONS:
        return "OPTIONS";
    case SOLAR_OS_RTSP_METHOD_DESCRIBE:
        return "DESCRIBE";
    case SOLAR_OS_RTSP_METHOD_SETUP:
        return "SETUP";
    case SOLAR_OS_RTSP_METHOD_PLAY:
        return "PLAY";
    case SOLAR_OS_RTSP_METHOD_TEARDOWN:
        return "TEARDOWN";
    case SOLAR_OS_RTSP_METHOD_GET_PARAMETER:
        return "GET_PARAMETER";
    case SOLAR_OS_RTSP_METHOD_UNSUPPORTED:
    default:
        return "unsupported";
    }
}
