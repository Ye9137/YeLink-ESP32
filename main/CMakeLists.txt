if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/moon_cat_gif.bin" OR
   NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/moon_cat_gif_data.h")
    message(FATAL_ERROR "Animation missing: run python3 scripts/prepare_demo_animation.py from the project root, or convert your own licensed GIF with scripts/prepare_cat_gif.py.")
endif()

idf_component_register(SRCS "pet_main.c" "main.c" "moon_cat.c" "moon_cat_gif.c" "fps_badge.c" "custom_animation.c" "wifi_share.c" "router_monitor.c" "router_ip4.c" "router_tcp_mss.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_gpio esp_driver_spi esp_wifi esp_netif esp_hw_support
                             esp_event nvs_flash mbedtls esp_http_client
                             esp_http_server lwip esp_timer
                    EMBED_TXTFILES "portal.html"
                    EMBED_FILES "moon_cat_gif.bin")
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=esp_netif_transmit"
    "-Wl,--wrap=esp_netif_transmit_wrap"
    "-Wl,--wrap=esp_netif_receive")
