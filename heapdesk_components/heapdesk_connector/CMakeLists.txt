idf_component_register(
    SRCS "src/heapdesk.c" "src/hd_metrics.c" "src/hd_commands.c" "src/hd_coredump.c" "src/hd_http.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    PRIV_REQUIRES mqtt esp_http_client esp_https_ota app_update espcoredump nvs_flash json
                  esp_timer spi_flash mbedtls esp_app_format esp_system vfs esp_wifi
)
