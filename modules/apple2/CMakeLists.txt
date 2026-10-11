cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(solaros_native_apple2)
include(elf_loader)
set(ELF_LIBS "-lgcc")
project_elf(solaros_native_apple2)
add_custom_command(OUTPUT solaros_native_apple2.vm.elf
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        solaros_native_apple2.app.elf solaros_native_apple2.vm.elf
    DEPENDS solaros_native_apple2.app.elf)
add_custom_target(typed_elf ALL DEPENDS solaros_native_apple2.vm.elf)
