#include "prompts.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "prompt";

/* src/prompts/<name>.pcm (English) and <name>_ca.pcm / <name>_es.pcm,
 * embedded by src/CMakeLists.txt (EMBED_FILES, no NUL). */
#define CLIP(name)                                                        \
    extern const uint8_t name##_start[] asm("_binary_" #name "_pcm_start"); \
    extern const uint8_t name##_end[] asm("_binary_" #name "_pcm_end");
#define CLIPS(name) CLIP(name) CLIP(name##_ca) CLIP(name##_es)
CLIPS(time_up)
CLIPS(unavailable)
CLIPS(no_answer)
CLIPS(no_wifi)
CLIPS(not_ready)
CLIPS(cant_call)
CLIPS(sleep_time)
CLIPS(secret_start)
CLIPS(secret_sent)

typedef struct {
    const char *name;
    const uint8_t *start, *end;
} clip_t;

/* One row per language; sfx is the file suffix (empty for English). */
#define ROW(id, name, sfx) [id] = {#name, name##sfx##_start, name##sfx##_end}
#define LANG_CLIPS(sfx)                                         \
    {                                                           \
        [PROMPT_NONE] = {"none", NULL, NULL},                   \
        ROW(PROMPT_TIME_UP, time_up, sfx),                      \
        ROW(PROMPT_UNAVAILABLE, unavailable, sfx),              \
        ROW(PROMPT_NO_ANSWER, no_answer, sfx),                  \
        ROW(PROMPT_NO_WIFI, no_wifi, sfx),                      \
        ROW(PROMPT_NOT_READY, not_ready, sfx),                  \
        ROW(PROMPT_CANT_CALL, cant_call, sfx),                  \
        ROW(PROMPT_SLEEP_TIME, sleep_time, sfx),                \
        ROW(PROMPT_SECRET_START, secret_start, sfx),            \
        ROW(PROMPT_SECRET_SENT, secret_sent, sfx),              \
    }

static const clip_t k_clips[PROMPT_LANG_COUNT][PROMPT_COUNT] = {
    [PROMPT_LANG_EN] = LANG_CLIPS(),
    [PROMPT_LANG_CA] = LANG_CLIPS(_ca),
    [PROMPT_LANG_ES] = LANG_CLIPS(_es),
};

static const char *const k_lang_codes[PROMPT_LANG_COUNT] = {
    [PROMPT_LANG_EN] = "en",
    [PROMPT_LANG_CA] = "ca",
    [PROMPT_LANG_ES] = "es",
};

/* Set by the setup server or at boot, read by the audio task when a prompt
 * starts: one atomic int, no lock. */
static _Atomic int s_lang = PROMPT_LANG_EN;

void prompt_set_lang(prompt_lang_t lang)
{
    if ((unsigned)lang >= PROMPT_LANG_COUNT) {
        lang = PROMPT_LANG_EN;
    }
    if (atomic_exchange(&s_lang, lang) != (int)lang) {
        ESP_LOGI(TAG, "language %s", k_lang_codes[lang]);
    }
}

prompt_lang_t prompt_lang(void)
{
    int l = atomic_load(&s_lang);
    return (unsigned)l < PROMPT_LANG_COUNT ? (prompt_lang_t)l : PROMPT_LANG_EN;
}

const char *prompt_lang_code(prompt_lang_t lang)
{
    return (unsigned)lang < PROMPT_LANG_COUNT ? k_lang_codes[lang] : k_lang_codes[PROMPT_LANG_EN];
}

prompt_lang_t prompt_lang_from_code(const char *code)
{
    for (int i = 0; code && i < PROMPT_LANG_COUNT; i++) {
        if (strcmp(code, k_lang_codes[i]) == 0) {
            return (prompt_lang_t)i;
        }
    }
    return PROMPT_LANG_COUNT;
}

/* Requests are numbered; the prompt is active until the audio task reports
 * the latest number done. The id is written before the number, so the audio
 * task never starts a request without its id. */
static _Atomic int s_req_id = PROMPT_NONE;
static _Atomic uint32_t s_req_seq;
static _Atomic uint32_t s_done_seq;

void prompt_play(prompt_id_t id)
{
    if (id <= PROMPT_NONE || id >= PROMPT_COUNT) {
        return;
    }
    ESP_LOGI(TAG, "prompt %s, %s (%" PRIu32 " ms)", k_clips[PROMPT_LANG_EN][id].name, prompt_lang_code(prompt_lang()),
             prompt_duration_ms(id));
    atomic_store(&s_req_id, id);
    atomic_fetch_add(&s_req_seq, 1);
}

bool prompt_active(void)
{
    return atomic_load(&s_req_seq) != atomic_load(&s_done_seq);
}

uint32_t prompt_duration_ms(prompt_id_t id)
{
    size_t n = 0;
    prompt_clip(id, &n);
    return (uint32_t)(n * 1000 / PROMPT_RATE);
}

const char *prompt_name(prompt_id_t id)
{
    return (unsigned)id < PROMPT_COUNT ? k_clips[PROMPT_LANG_EN][id].name : "?";
}

prompt_id_t prompt_poll(uint32_t *req)
{
    uint32_t seq = atomic_load(&s_req_seq);
    if (seq == *req) {
        return PROMPT_NONE;
    }
    *req = seq;
    return (prompt_id_t)atomic_load(&s_req_id);
}

void prompt_done(uint32_t req)
{
    atomic_store(&s_done_seq, req);
}

const uint8_t *prompt_clip(prompt_id_t id, size_t *n)
{
    if (id <= PROMPT_NONE || id >= PROMPT_COUNT) {
        *n = 0;
        return NULL;
    }
    const clip_t *c = &k_clips[prompt_lang()][id];
    *n = (size_t)(c->end - c->start) / 2;
    return c->start;
}
