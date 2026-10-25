/*
 * Toddler phone: the button places a real phone call over SIP.
 *
 * ESP32-S3 is USB host (native port, GPIO 19/20) to a USB-C telephone
 * handset, and a SIP client over WiFi to a Twilio SIP Domain. Setup needs no
 * toolchain: WiFi over Improv Serial (browser flasher), then Twilio from the
 * phone's own web page (http://kidphone.local/). Up to four contact buttons
 * (APP_BUTTON_GPIOS) with one LED each (APP_CONTACT_LED_GPIOS):
 *   idle, press i             call contact i (a spoken prompt, then a short
 *                             busy tone, if it is bedtime, the slot is empty
 *                             or the contact is outside their available hours)
 *   in a call, press any      hang up (CANCEL while ringing, BYE when answered)
 *   while a prompt plays, and APP_PRESS_COOLDOWN_MS after a call ends or
 *   fails, presses are ignored (button mashing)
 * A contact button acts once, on the press edge. Holding it does nothing
 * more and never repeats; the next action needs a release and a new press.
 * The board's BOOT button (APP_BOOT_GPIO, out of a child's reach):
 *   idle, short press         cycle the stage 1 audio test modes A-D
 *   hold 10 s                 factory reset (erase config, reboot into setup);
 *                             the status LED warns from APP_BUTTON_LONG_MS on
 * The setup page can also factory reset (POST /api/factory_reset).
 * Console is UART0 (the board's COM port), shared with Improv Serial.
 */
#include <stdatomic.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "audio_engine.h"
#include "config_store.h"
#include "improv.h"
#include "contact_leds.h"
#include "modes.h"
#include "prompts.h"
#include "rtp.h"
#include "schedule.h"
#include "setup_server.h"
#include "sip.h"
#include "status_led.h"
#include "usb_audio.h"
#include "wifi.h"

static const char *TAG = "main";

/* Test mode chosen with BOOT short presses while idle, or -1 for the normal idle
 * sound (the status tone, see app_mode_get). */
static _Atomic int s_test_mode = -1;

/* Busy tone until then: after a failed call (APP_BUSY_TONE_MS) or a button
 * that cannot call (APP_SHORT_BUSY_MS), counted from the end of the prompt
 * that explains it. */
static _Atomic int64_t s_busy_until_ms;

/* Contact button presses are ignored until then (APP_PRESS_COOLDOWN_MS after a call
 * ends or fails). */
static _Atomic int64_t s_cooldown_until_ms;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

app_mode_t app_mode_get(void)
{
    call_state_t cs = sip_call_state();
    if (cs == CALL_RINGING && !rtp_media_received()) {
        return MODE_RINGBACK; /* Twilio sends 180 with no media: ring locally */
    }
    if (cs != CALL_IDLE) {
        return MODE_CALL;
    }
    if (prompt_active()) {
        return MODE_PROMPT;
    }
    int t = atomic_load(&s_test_mode);
    if (t >= 0) {
        return (app_mode_t)t;
    }
#if APP_IDLE_DIAL_TONE
    /* Idle: the handset tells the status. Dial tone only when really ready. */
    if (!wifi_is_connected()) {
        return MODE_NO_WIFI;
    }
    if (now_ms() < atomic_load(&s_busy_until_ms)) {
        return MODE_BUSY;
    }
    if (!config_is_provisioned()) {
        return MODE_SETUP_NEEDED;
    }
    return MODE_C_DIAL_TONE;
#else
    /* Idle: silent (no hook switch yet). Only the busy tone that follows a
     * press or a failed call; the contact lights show the status. */
    if (now_ms() < atomic_load(&s_busy_until_ms)) {
        return MODE_BUSY;
    }
    return MODE_D_SILENCE;
#endif
}

const char *app_mode_name(app_mode_t m)
{
    switch (m) {
    case MODE_A_WIDEBAND_LOOPBACK: return "wideband loopback (mic -> earpiece)";
    case MODE_B_TELEPHONE_LOOPBACK: return "telephone loopback (8 kHz + G.711 mu-law)";
    case MODE_C_DIAL_TONE: return "US dial tone (350 + 440 Hz)";
    case MODE_D_SILENCE: return "silence";
    case MODE_CALL: return "call (mic -> RTP, RTP -> earpiece)";
    case MODE_RINGBACK: return "US ringback (440 + 480 Hz, 2 s on / 4 s off)";
    case MODE_SETUP_NEEDED: return "setup needed (reorder, 480 + 620 Hz, 250/250 ms)";
    case MODE_BUSY: return "busy (480 + 620 Hz, 500/500 ms)";
    case MODE_NO_WIFI: return "no WiFi (slow beep)";
    case MODE_PROMPT: return "spoken prompt";
    default: return "?";
    }
}

const char *app_mode_short(app_mode_t m)
{
    static const char *k[] = {"A", "B", "C", "D", "CALL", "RING", "SETUP", "BUSY", "NOWIFI", "PROMPT"};
    _Static_assert(sizeof(k) / sizeof(k[0]) == MODE_COUNT, "mode names out of sync");
    return (unsigned)m < MODE_COUNT ? k[m] : "?";
}

/* BOOT short press while idle: auto -> A -> B -> C -> D -> auto. */
static void cycle_test_mode(void)
{
    int t = atomic_load(&s_test_mode) + 1;
    if (t >= MODE_TEST_COUNT) {
        t = -1;
    }
    atomic_store(&s_test_mode, t);
    if (t < 0) {
        ESP_LOGI(TAG, "BOOT: test mode off, idle sound = %s", app_mode_name(app_mode_get()));
    } else {
        ESP_LOGI(TAG, "BOOT: test mode %s, %s", app_mode_short((app_mode_t)t), app_mode_name((app_mode_t)t));
    }
}

/* Tell the child what happened: a spoken prompt, then (busy_ms > 0) the
 * busy tone for busy_ms, then the normal idle sound (silence, or the status
 * tone with APP_IDLE_DIAL_TONE). */
static void feedback(prompt_id_t p, int busy_ms)
{
    atomic_store(&s_test_mode, -1);
    atomic_store(&s_busy_until_ms, busy_ms > 0 ? now_ms() + prompt_duration_ms(p) + busy_ms : 0);
    prompt_play(p);
}

/* What the phone says after a call attempt, from sip.c's classification
 * (sip_result_t kind). "" = the call went fine: nothing to say. */
static prompt_id_t prompt_for_result(const sip_result_t *r)
{
    static const struct {
        const char *kind;
        prompt_id_t prompt;
    } map[] = {
        {"timeup", PROMPT_TIME_UP},       {"busy", PROMPT_NO_ANSWER},       {"noanswer", PROMPT_NO_ANSWER},
        {"network", PROMPT_NO_WIFI},      {"setup", PROMPT_NOT_READY},      {"login", PROMPT_CANT_CALL},
        {"domain", PROMPT_CANT_CALL},     {"unreachable", PROMPT_CANT_CALL}, {"other", PROMPT_CANT_CALL},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(r->kind, map[i].kind) == 0) {
            return map[i].prompt;
        }
    }
    return PROMPT_NONE;
}

/* Called from the button task: a new call result (button or setup page
 * call) starts the press cooldown and the prompt. A failed call is followed
 * by the busy tone as before. */
static void check_call_result(uint32_t *seen)
{
    uint32_t seq = sip_result_seq();
    if (seq == *seen) {
        return;
    }
    *seen = seq;
    sip_result_t r;
    sip_last_result(&r);
    atomic_store(&s_cooldown_until_ms, now_ms() + APP_PRESS_COOLDOWN_MS);
    prompt_id_t p = prompt_for_result(&r);
    bool busy = r.failed || strcmp(r.kind, "unreachable") == 0;
    if (p != PROMPT_NONE || busy) {
        ESP_LOGI(TAG, "call result \"%s\": prompt %s%s", r.kind, prompt_name(p), busy ? ", then busy tone" : "");
        feedback(p, busy ? APP_BUSY_TONE_MS : 0);
    }
}

static void on_press_idle(int idx)
{
    if (prompt_active()) {
        ESP_LOGI(TAG, "BUTTON %d: ignored, prompt playing", idx);
        return;
    }
    if (now_ms() < atomic_load(&s_cooldown_until_ms)) {
        ESP_LOGI(TAG, "BUTTON %d: ignored, cooldown after the last call", idx);
        return;
    }
    /* Bedtime first: whatever else is wrong, the child only needs to hear
     * that it is sleep time. Off until the clock has synced. */
    cfg_bedtime_t bed;
    bedtime_state_t bst;
    config_get_bedtime(&bed);
    schedule_bedtime_eval(&bed, &bst);
    if (bst.active) {
        ESP_LOGI(TAG, "BUTTON %d: bedtime, busy tone", idx);
        feedback(PROMPT_SLEEP_TIME, APP_SHORT_BUSY_MS);
        return;
    }
    if (!wifi_is_connected()) {
        ESP_LOGW(TAG, "BUTTON %d: no WiFi yet, cannot call", idx);
        feedback(PROMPT_NO_WIFI, 0);
        return;
    }
    if (!config_is_provisioned()) {
        ESP_LOGW(TAG, "BUTTON %d: not set up yet, open http://%s.local/", idx, APP_HOSTNAME);
        feedback(PROMPT_NOT_READY, 0);
        return;
    }
    cfg_contact_t c;
    if (!config_get_contact(idx, &c)) {
        ESP_LOGW(TAG, "BUTTON %d: no contact in this slot, busy tone", idx);
        feedback(PROMPT_NOT_READY, APP_SHORT_BUSY_MS);
        return;
    }
    sched_state_t st;
    schedule_eval(&c.sched, &st);
    if (!st.available) {
        ESP_LOGI(TAG, "BUTTON %d: %s is outside their available hours, busy tone", idx, c.name);
        feedback(PROMPT_UNAVAILABLE, APP_SHORT_BUSY_MS);
        return;
    }
    if (st.scheduled && !schedule_time_synced()) {
        ESP_LOGW(TAG, "BUTTON %d: clock not synced, calling without checking the schedule", idx);
    }
    ESP_LOGI(TAG, "BUTTON %d: call %s", idx, c.name);
    atomic_store(&s_test_mode, -1); /* back to the normal idle sound after the call */
    atomic_store(&s_busy_until_ms, 0);
    sip_call_contact(idx);
}

/* Stop any call and erase the config (NVS namespace plus the WiFi driver's
 * own copy). The caller reboots. */
static void factory_reset_erase(const char *source)
{
    ESP_LOGW(TAG, "FACTORY RESET (%s)", source);
    sip_call_hangup();
    config_factory_reset();
    esp_wifi_restore(); /* the WiFi driver keeps its own copy in NVS */
}

void app_factory_reset(const char *source)
{
    factory_reset_erase(source);
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

/* Debounced active-low input, polled every APP_BUTTON_POLL_MS. Returns +1 on
 * the press edge, -1 on the release edge, 0 otherwise. The level must be
 * stable for APP_BUTTON_DEBOUNCE_MS before it counts. */
typedef struct {
    int gpio;
    int stable;   /* debounced level, 1 = released */
    int last_raw;
    int same_ms;
    int held_ms;  /* since the press edge; kept after release = press length */
} button_t;

static int button_debounce(button_t *b)
{
    int raw = gpio_get_level(b->gpio);
    if (raw == b->last_raw) {
        if (b->same_ms < APP_BUTTON_DEBOUNCE_MS) {
            b->same_ms += APP_BUTTON_POLL_MS;
        }
    } else {
        b->same_ms = 0;
        b->last_raw = raw;
    }
    int edge = 0;
    if (b->same_ms >= APP_BUTTON_DEBOUNCE_MS && raw != b->stable) {
        b->stable = raw;
        edge = raw == 0 ? 1 : -1;
        if (edge == 1) {
            b->held_ms = 0;
        }
    }
    if (b->stable == 0) {
        b->held_ms += APP_BUTTON_POLL_MS;
    }
    return edge;
}

/*
 * Contact buttons: one action per press, on the press edge. Idle: call that
 * contact (subject to the prompt / cooldown / schedule rules). In a call:
 * hang up. Holding does nothing more, and a held button never repeats: the
 * next action needs a release and a new press.
 */
static void contact_button_poll(int idx, button_t *b)
{
    if (button_debounce(b) != 1) {
        return;
    }
    if (sip_call_state() != CALL_IDLE) {
        ESP_LOGI(TAG, "BUTTON %d: hang up (state %s)", idx, sip_call_state_name(sip_call_state()));
        sip_call_hangup();
    } else {
        on_press_idle(idx);
    }
}

/*
 * BOOT button (GPIO 0): parent-only, on the board. A short press (released
 * before APP_BUTTON_LONG_MS) while idle cycles the test modes. From
 * APP_BUTTON_LONG_MS on the status LED warns; at APP_FACTORY_RESET_MS the
 * config is erased, and the reboot waits for the release (a strapping pin
 * held low through a reset could start the ROM downloader).
 */
static void boot_button_poll(button_t *b)
{
    int edge = button_debounce(b);
    if (edge == -1) {
        status_led_reset_warning(false);
        if (b->held_ms < APP_BUTTON_LONG_MS && sip_call_state() == CALL_IDLE) {
            cycle_test_mode();
        }
        return;
    }
    if (b->stable != 0) {
        return;
    }
    status_led_reset_warning(b->held_ms >= APP_BUTTON_LONG_MS);
    if (b->held_ms < APP_FACTORY_RESET_MS) {
        return;
    }
    factory_reset_erase("BOOT button held");
    int waited = 0, released_ms = 0;
    while (released_ms < APP_BUTTON_DEBOUNCE_MS && waited < APP_BOOT_RELEASE_WAIT_MS) {
        released_ms = gpio_get_level(b->gpio) ? released_ms + APP_BUTTON_POLL_MS : 0;
        waited += APP_BUTTON_POLL_MS;
        vTaskDelay(pdMS_TO_TICKS(APP_BUTTON_POLL_MS));
    }
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

static void button_task(void *arg)
{
    (void)arg;
    static const int gpios[APP_NUM_BUTTONS] = APP_BUTTON_GPIOS;
    button_t btn[APP_NUM_BUTTONS];
    uint64_t mask = 0;
    for (int i = 0; i < APP_NUM_BUTTONS; i++) {
        btn[i] = (button_t){.gpio = gpios[i], .stable = 1, .last_raw = 1};
        mask |= 1ULL << gpios[i];
    }
    const gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    /* BOOT: input only, never driven. The board has its own pull-up; the
     * internal one is the same direction and harmless. */
    const gpio_config_t boot_cfg = {
        .pin_bit_mask = 1ULL << APP_BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&boot_cfg));
    button_t boot = {.gpio = APP_BOOT_GPIO, .stable = 1, .last_raw = 1};
    uint32_t result_seen = sip_result_seq();
    while (1) {
        check_call_result(&result_seen);
        for (int i = 0; i < APP_NUM_BUTTONS; i++) {
            contact_button_poll(i, &btn[i]);
        }
        boot_button_poll(&boot);
        vTaskDelay(pdMS_TO_TICKS(APP_BUTTON_POLL_MS));
    }
}

void app_main(void)
{
    static const int gpios[APP_NUM_BUTTONS] = APP_BUTTON_GPIOS;
    ESP_LOGI(TAG, "kid_phone: button i calls contact i");
    ESP_LOGI(TAG, "Buttons on GPIO %d, %d, %d, %d (to GND): press = call / hang up, holding does nothing", gpios[0],
             gpios[1], gpios[2], gpios[3]);
    ESP_LOGI(TAG, "BOOT (GPIO %d): short press while idle = cycle test modes A-D, hold %d ms = factory reset",
             APP_BOOT_GPIO, APP_FACTORY_RESET_MS);

    config_store_init();
    wifi_start();
    improv_start();
    setup_server_start();
    status_led_start();
    schedule_start();
    contact_leds_start();
    sip_start();
    usb_audio_start();
    audio_engine_start();
    xTaskCreatePinnedToCore(button_task, "button", 3072, NULL, 3, NULL, 0);
}
