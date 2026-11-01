#include <stdio.h>
#include <string.h>
#include "mbedtls/base64.h"
#include "sdkconfig.h"
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif
#include "hd_internal.h"

void hd_http_config(esp_http_client_config_t *c, const char *url)
{
    memset(c, 0, sizeof(*c));
    c->url = url;
    c->timeout_ms = 15000;
    c->buffer_size = 2048;
    c->buffer_size_tx = 1024;
    if (hd_cfg.ca_cert_pem) {
        c->cert_pem = hd_cfg.ca_cert_pem;
    } else {
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        c->crt_bundle_attach = esp_crt_bundle_attach;
#endif
    }
}

bool hd_url_allowed(const char *url)
{
    if (!url) {
        return false;
    }
    if (strncmp(url, "https://", 8) == 0 || strncmp(url, "mqtts://", 8) == 0) {
        return true;
    }
#ifdef CONFIG_HEAPDESK_INSECURE_TLS_ALLOWED
    return strncmp(url, "http://", 7) == 0 || strncmp(url, "mqtt://", 7) == 0;
#else
    return false;
#endif
}

void hd_hex(const uint8_t *in, size_t n, char *out)
{
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = hx[in[i] >> 4];
        out[2 * i + 1] = hx[in[i] & 15];
    }
    out[2 * n] = '\0';
}

esp_err_t hd_basic_auth(esp_http_client_handle_t h)
{
    char raw[192], b64[272], hdr[288];
    int n = snprintf(raw, sizeof(raw), "%s:%s", hd_cfg.device_id, hd_cfg.device_secret);
    if (n <= 0 || n >= (int)sizeof(raw)) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t olen = 0;
    if (mbedtls_base64_encode((unsigned char *)b64, sizeof(b64), &olen, (unsigned char *)raw, n) != 0) {
        return ESP_FAIL;
    }
    b64[olen] = '\0';
    snprintf(hdr, sizeof(hdr), "Basic %s", b64);
    memset(raw, 0, sizeof(raw));
    return esp_http_client_set_header(h, "Authorization", hdr);
}
