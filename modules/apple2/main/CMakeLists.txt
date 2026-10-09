idf_component_register(SRCS "apple2.c" "machine.c" "video.c" "vendor/puce6502.c"
    INCLUDE_DIRS "../../../include" "." "vendor")
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
set_source_files_properties("vendor/puce6502.c" PROPERTIES
    COMPILE_OPTIONS "-fjump-tables;-ftree-switch-conversion")
