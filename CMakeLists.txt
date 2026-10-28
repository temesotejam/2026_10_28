cmake_minimum_required(VERSION 3.13)
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(sbus_uart_bridge C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(sbus_uart_bridge
    pico/sbus_uart_bridge.cpp
)
pico_generate_pio_header(sbus_uart_bridge ${CMAKE_CURRENT_LIST_DIR}/pico/uart_tx.pio)
target_link_libraries(sbus_uart_bridge
    pico_stdlib hardware_uart hardware_irq hardware_pio hardware_clocks hardware_sync
)
pico_enable_stdio_usb(sbus_uart_bridge 1)
pico_enable_stdio_uart(sbus_uart_bridge 0)
pico_add_extra_outputs(sbus_uart_bridge)
