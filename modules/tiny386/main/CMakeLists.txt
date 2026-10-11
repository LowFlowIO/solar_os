set(core_sources vendor/i386.c vendor/i8042.c vendor/i8254.c vendor/i8259.c
    vendor/ide.c vendor/pc.c vendor/misc.c vendor/pci.c vendor/vga.c
    vendor/vgafont.c vendor/i8257.c vendor/sb16.c vendor/pcspk.c vendor/adlib.c)
set(bios_source "${CMAKE_CURRENT_BINARY_DIR}/bios_data.c")
idf_component_register(SRCS tiny386.c port.c "${bios_source}" ${core_sources}
    INCLUDE_DIRS "." "../../../include")
add_custom_command(OUTPUT "${bios_source}"
    COMMAND ${PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/embed_bios.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../bios" "${bios_source}"
    DEPENDS "../scripts/embed_bios.py" "../bios/bios.bin" "../bios/vgabios.bin")
target_compile_definitions(${COMPONENT_LIB} PRIVATE BPP=16)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -g0)
set_source_files_properties(${core_sources} PROPERTIES
    COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/port.h;-Wno-all;-fjump-tables;-ftree-switch-conversion")
