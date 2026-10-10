FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.c)
idf_component_register(SRCS ${app_sources}
                       INCLUDE_DIRS "."
                       REQUIRES usb esp_timer driver g711 resampler
                                esp_wifi esp_netif esp_event nvs_flash lwip mbedtls)
