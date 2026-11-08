FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.c)
# setup_page.html is the phone's setup web page; voice.js is the Twilio
# Function source, uploaded to the parent's account during provisioning
# (one source of truth: twilio/voice.js). prompts/*.pcm are the spoken
# prompts (16 kHz mono s16le, made by scripts/gen_prompts.py), see prompts.c:
# <name>.pcm English, <name>_<lang>.pcm the others (_ca Catalan, _es Spanish,
# _fr French, _it Italian, _nl Dutch, _de German, _vi Vietnamese, _pt_br
# Brazilian Portuguese).
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
                                   "prompts/time_up_fr.pcm" "prompts/unavailable_fr.pcm" "prompts/no_answer_fr.pcm"
                                   "prompts/no_wifi_fr.pcm" "prompts/not_ready_fr.pcm" "prompts/cant_call_fr.pcm"
                                   "prompts/sleep_time_fr.pcm" "prompts/secret_start_fr.pcm" "prompts/secret_sent_fr.pcm"
                                   "prompts/time_up_it.pcm" "prompts/unavailable_it.pcm" "prompts/no_answer_it.pcm"
                                   "prompts/no_wifi_it.pcm" "prompts/not_ready_it.pcm" "prompts/cant_call_it.pcm"
                                   "prompts/sleep_time_it.pcm" "prompts/secret_start_it.pcm" "prompts/secret_sent_it.pcm"
                                   "prompts/time_up_nl.pcm" "prompts/unavailable_nl.pcm" "prompts/no_answer_nl.pcm"
                                   "prompts/no_wifi_nl.pcm" "prompts/not_ready_nl.pcm" "prompts/cant_call_nl.pcm"
                                   "prompts/sleep_time_nl.pcm" "prompts/secret_start_nl.pcm" "prompts/secret_sent_nl.pcm"
                                   "prompts/time_up_de.pcm" "prompts/unavailable_de.pcm" "prompts/no_answer_de.pcm"
                                   "prompts/no_wifi_de.pcm" "prompts/not_ready_de.pcm" "prompts/cant_call_de.pcm"
                                   "prompts/sleep_time_de.pcm" "prompts/secret_start_de.pcm" "prompts/secret_sent_de.pcm"
                                   "prompts/time_up_vi.pcm" "prompts/unavailable_vi.pcm" "prompts/no_answer_vi.pcm"
                                   "prompts/no_wifi_vi.pcm" "prompts/not_ready_vi.pcm" "prompts/cant_call_vi.pcm"
                                   "prompts/sleep_time_vi.pcm" "prompts/secret_start_vi.pcm" "prompts/secret_sent_vi.pcm"
                                   "prompts/time_up_pt_br.pcm" "prompts/unavailable_pt_br.pcm" "prompts/no_answer_pt_br.pcm"
                                   "prompts/no_wifi_pt_br.pcm" "prompts/not_ready_pt_br.pcm" "prompts/cant_call_pt_br.pcm"
                                   "prompts/sleep_time_pt_br.pcm" "prompts/secret_start_pt_br.pcm" "prompts/secret_sent_pt_br.pcm"
                                   "${CMAKE_SOURCE_DIR}/twilio/no_answer.wav" "${CMAKE_SOURCE_DIR}/twilio/no_answer_ca.wav"
                                   "${CMAKE_SOURCE_DIR}/twilio/no_answer_es.wav" "${CMAKE_SOURCE_DIR}/twilio/no_answer_fr.wav"
                                   "${CMAKE_SOURCE_DIR}/twilio/no_answer_it.wav" "${CMAKE_SOURCE_DIR}/twilio/no_answer_nl.wav"
                                   "${CMAKE_SOURCE_DIR}/twilio/no_answer_de.wav" "${CMAKE_SOURCE_DIR}/twilio/no_answer_vi.wav"
                                   "${CMAKE_SOURCE_DIR}/twilio/no_answer_pt_br.wav"
                       REQUIRES usb esp_timer driver g711 resampler
                                esp_wifi esp_netif esp_event nvs_flash lwip mbedtls
                                esp_http_client esp_http_server json esp_driver_uart esp_app_format esp-tls)
