idf_component_register(
    SRCS "stb_image_port.c" "jpeg_fast.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES heap esp_rom
)
