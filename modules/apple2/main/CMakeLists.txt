set(rom_source "${CMAKE_CURRENT_BINARY_DIR}/roms_data.c")
idf_component_register(SRCS "apple2.c" "machine.c" "video.c" "vendor/puce6502.c" "${rom_source}"
    INCLUDE_DIRS "../../../include" "." "vendor")
add_custom_command(OUTPUT "${rom_source}"
    COMMAND ${PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/embed_roms.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../rom" "${rom_source}"
    DEPENDS "../scripts/embed_roms.py" "../rom/appleII+.rom" "../rom/diskII.rom" "roms.h")
# The module ELF is stripped. Avoid emitting DWARF that triggers a crash in
# the Xtensa linker when it strips debug information from a shared ELF.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -g0)
set_source_files_properties("vendor/puce6502.c" PROPERTIES
    COMPILE_OPTIONS "-fjump-tables;-ftree-switch-conversion")
