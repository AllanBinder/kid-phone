FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.c)
# setup_page.html is the phone's setup web page; voice.js is the Twilio
# Function source, uploaded to the parent's account during provisioning
# (one source of truth: twilio/voice.js). prompts/*.pcm are the spoken
# prompts (16 kHz mono s16le, made by scripts/gen_prompts.py), see prompts.c.
# twilio/no_answer.wav is the same voice for Twilio to play on the call
# (8 kHz mu-law), uploaded as a Serverless Asset during provisioning.
idf_component_register(SRCS ${app_sources}
                       INCLUDE_DIRS "."
                       EMBED_TXTFILES "setup_page.html" "${CMAKE_SOURCE_DIR}/twilio/voice.js"
                       EMBED_FILES "prompts/time_up.pcm" "prompts/unavailable.pcm" "prompts/no_answer.pcm"
                                   "prompts/no_wifi.pcm" "prompts/not_ready.pcm" "prompts/cant_call.pcm"
                                   "prompts/sleep_time.pcm"
                                   "${CMAKE_SOURCE_DIR}/twilio/no_answer.wav"
                       REQUIRES usb esp_timer driver g711 resampler
                                esp_wifi esp_netif esp_event nvs_flash lwip mbedtls
                                esp_http_client esp_http_server json esp_driver_uart esp_app_format)
