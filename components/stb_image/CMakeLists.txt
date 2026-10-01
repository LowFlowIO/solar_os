set(jpeg_private_requires heap esp_rom)
if(IDF_TARGET STREQUAL "esp32s3")
    list(APPEND jpeg_private_requires espressif__esp_new_jpeg)
endif()
idf_component_register(
    SRCS "stb_image_port.c" "jpeg_fast.c" "jpeg_simd.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "../../src"
    PRIV_REQUIRES ${jpeg_private_requires}
)
