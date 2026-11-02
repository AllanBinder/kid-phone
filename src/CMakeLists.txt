FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.c)
# setup_page.html is the phone's setup web page; voice.js is the Twilio
# Function source, uploaded to the parent's account during provisioning
# (one source of truth: twilio/voice.js). prompts/*.pcm are the spoken
# prompts (16 kHz mono s16le, made by scripts/gen_prompts.py), see prompts.c:
# <name>.pcm English, <name>_ca.pcm Catalan, <name>_es.pcm Spanish.
# twilio/no_answer*.wav are the same voices for Twilio to play on the call
# (8 kHz mu-law), uploaded as Serverless Assets during provisioning.
# Both lists must match board_build.embed_* in platformio.ini.
idf_component_register(SRCS ${app_sources}
                       INCLUDE_DIRS "."
                       EMBED_TXTFILES "setup_page.html" "${CMAKE_SOURCE_DIR}/twilio/voice.js"
                       EMBED_FILES "prompts/time_up.pcm" "prompts/unavailable.pcm" "prompts/no_answer.pcm"
                                   "prompts/no_wifi.pcm" "prompts/not_ready.pcm" "prompts/cant_call.pcm"
                                   "prompts/sleep_time.pcm" "prompts/secret_start.pcm" "prompts/secret_sent.pcm"
                                   "prompts/time_up_ca.pcm" "prompts/unavailable_ca.pcm" "prompts/no_answer_ca.pcm"
                                   "prompts/no_wifi_ca.pcm" "prompts/not_ready_ca.pcm" "prompts/cant_call_ca.pcm"
                                   "prompts/sleep_time_ca.pcm" "prompts/secret_start_ca.pcm" "prompts/secret_sent_ca.pcm"
                                   "prompts/time_up_es.pcm" "prompts/unavailable_es.pcm" "prompts/no_answer_es.pcm"
                                   "prompts/no_wifi_es.pcm" "prompts/not_ready_es.pcm" "prompts/cant_call_es.pcm"
                                   "prompts/sleep_time_es.pcm" "prompts/secret_start_es.pcm" "prompts/secret_sent_es.pcm"
                                   "${CMAKE_SOURCE_DIR}/twilio/no_answer.wav" "${CMAKE_SOURCE_DIR}/twilio/no_answer_ca.wav"
                                   "${CMAKE_SOURCE_DIR}/twilio/no_answer_es.wav"
                       REQUIRES usb esp_timer driver g711 resampler
                                esp_wifi esp_netif esp_event nvs_flash lwip mbedtls
                                esp_http_client esp_http_server json esp_driver_uart esp_app_format esp-tls)
