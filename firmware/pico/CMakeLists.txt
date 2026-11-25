cmake_minimum_required(VERSION 3.13)

if(NOT DEFINED PICO_SDK_PATH)
    set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}")
endif()
if(NOT EXISTS "${PICO_SDK_PATH}/external/pico_sdk_import.cmake")
    message(FATAL_ERROR "Set PICO_SDK_PATH to a complete Raspberry Pi Pico SDK checkout")
endif()

# La Pico H original utiliza la definición pico del SDK y la plataforma RP2040.
set(PICO_BOARD pico CACHE STRING "Board for the original Pico H")
set(PICO_PLATFORM rp2040 CACHE STRING "Platform for the original Pico H")
if(NOT PICO_BOARD STREQUAL "pico" OR NOT PICO_PLATFORM STREQUAL "rp2040")
    message(FATAL_ERROR "This firmware targets the original RP2040 Pico H: PICO_BOARD=pico")
endif()

foreach(identifier TYPIST_USB_VID TYPIST_USB_PID)
    if(NOT DEFINED ${identifier} OR NOT "${${identifier}}" MATCHES "^(0x[0-9A-Fa-f]+|[0-9]+)$")
        message(FATAL_ERROR "Set ${identifier} explicitly; no USB identity is assigned by default")
    endif()
    math(EXPR identifierValue "${${identifier}}")
    if(identifierValue LESS 1 OR identifierValue GREATER 65534)
        message(FATAL_ERROR "${identifier} must be between 1 and 65534")
    endif()
endforeach()

# bMaxPower declara la solicitud del dispositivo USB, no la alimentación de los servos.
if(NOT DEFINED TYPIST_USB_POWER_MA OR NOT "${TYPIST_USB_POWER_MA}" MATCHES "^[0-9]+$")
    message(FATAL_ERROR "Set TYPIST_USB_POWER_MA explicitly for the USB device power budget")
endif()
math(EXPR powerRemainder "${TYPIST_USB_POWER_MA} % 2")
if(TYPIST_USB_POWER_MA LESS 2 OR TYPIST_USB_POWER_MA GREATER 500 OR NOT powerRemainder EQUAL 0)
    message(FATAL_ERROR "USB 2.0 bMaxPower requires an even budget from 2 to 500 mA")
endif()

include("${PICO_SDK_PATH}/external/pico_sdk_import.cmake")
project(typist_pico C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
pico_sdk_init()
if(NOT TARGET tinyusb_device)
    message(FATAL_ERROR "Initialize the Pico SDK lib/tinyusb submodule before building")
endif()

add_executable(typist_pico main.c statusProtocol.c usbDescriptors.c)
# El SDK incluye soporte C++, pero el firmware del proyecto no requiere enlazar libstdc++.
set_target_properties(typist_pico PROPERTIES LINKER_LANGUAGE C)
target_include_directories(typist_pico PRIVATE ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../../include)
target_compile_definitions(typist_pico PRIVATE
    TYPIST_USB_VID=${TYPIST_USB_VID} TYPIST_USB_PID=${TYPIST_USB_PID}
    TYPIST_USB_POWER_MA=${TYPIST_USB_POWER_MA})
# Las advertencias del proyecto se aplican a nuestras fuentes, sin alterar las del SDK.
set_source_files_properties(main.c statusProtocol.c usbDescriptors.c PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Wpedantic")
target_link_libraries(typist_pico PRIVATE pico_stdlib pico_unique_id tinyusb_device)
pico_enable_stdio_usb(typist_pico 0)
pico_enable_stdio_uart(typist_pico 0)
pico_add_extra_outputs(typist_pico)
