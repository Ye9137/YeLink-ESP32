# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_test)

# Copyright (c) 2026 Ye；默认 512 条映射，并及时回收未建立/已结束的连接。
idf_component_get_property(ye_lwip_library lwip COMPONENT_LIB)
math(EXPR ye_udp_timeout_ms "${CONFIG_YE_NAPT_UDP_TIMEOUT_SECONDS} * 1000")
math(EXPR ye_tcp_transient_ms "${CONFIG_YE_NAPT_TCP_TRANSIENT_SECONDS} * 1000")
target_compile_definitions(${ye_lwip_library} PRIVATE
    IP_NAPT_MAX=${CONFIG_YE_NAPT_CAPACITY}
    IP_NAPT_TIMEOUT_MS_UDP=${ye_udp_timeout_ms}
    IP_NAPT_TIMEOUT_MS_TCP_DISCON=${ye_tcp_transient_ms}
    "ESP_IDF_LWIP_HOOK_FILENAME=\"ye_lwip_hooks.h\"")
target_include_directories(${ye_lwip_library} PRIVATE "${PROJECT_DIR}/main")
