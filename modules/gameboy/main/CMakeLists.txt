idf_component_register(SRCS "gameboy.c" "apu.c"
    "solar_os_gameboy_rom.c" "solar_os_gameboy_video.c"
    INCLUDE_DIRS "." "../../../include" "../../../src/services")
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -g0 -fjump-tables -ftree-switch-conversion)
