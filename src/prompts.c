#include "prompts.h"

#include <inttypes.h>
#include <stdatomic.h>

#include "esp_log.h"

static const char *TAG = "prompt";

/* src/prompts/<name>.pcm, embedded by src/CMakeLists.txt (EMBED_FILES, no NUL). */
#define CLIP(name)                                                        \
    extern const uint8_t name##_start[] asm("_binary_" #name "_pcm_start"); \
    extern const uint8_t name##_end[] asm("_binary_" #name "_pcm_end");
CLIP(time_up)
CLIP(unavailable)
CLIP(no_answer)
CLIP(no_wifi)
CLIP(not_ready)
CLIP(cant_call)

typedef struct {
    const char *name;
    const uint8_t *start, *end;
} clip_t;

static const clip_t k_clips[PROMPT_COUNT] = {
    [PROMPT_NONE] = {"none", NULL, NULL},
    [PROMPT_TIME_UP] = {"time_up", time_up_start, time_up_end},
    [PROMPT_UNAVAILABLE] = {"unavailable", unavailable_start, unavailable_end},
    [PROMPT_NO_ANSWER] = {"no_answer", no_answer_start, no_answer_end},
    [PROMPT_NO_WIFI] = {"no_wifi", no_wifi_start, no_wifi_end},
    [PROMPT_NOT_READY] = {"not_ready", not_ready_start, not_ready_end},
    [PROMPT_CANT_CALL] = {"cant_call", cant_call_start, cant_call_end},
};

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
    ESP_LOGI(TAG, "prompt %s (%" PRIu32 " ms)", k_clips[id].name, prompt_duration_ms(id));
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
    return (unsigned)id < PROMPT_COUNT ? k_clips[id].name : "?";
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
    *n = (size_t)(k_clips[id].end - k_clips[id].start) / 2;
    return k_clips[id].start;
}
