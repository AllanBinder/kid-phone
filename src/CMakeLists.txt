FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.c)
# setup_page.html is the phone's setup web page; voice.js is the Twilio
# Function source, uploaded to the parent's account during provisioning
# (one source of truth: twilio/voice.js).
idf_component_register(SRCS ${app_sources}
                       INCLUDE_DIRS "."
                       EMBED_TXTFILES "setup_page.html" "${CMAKE_SOURCE_DIR}/twilio/voice.js"
                       REQUIRES usb esp_timer driver g711 resampler
                                esp_wifi esp_netif esp_event nvs_flash lwip mbedtls
                                esp_http_client esp_http_server json esp_driver_uart esp_app_format)
