#include "wav.h"

#include <string.h>

/* Peak target: -3 dBFS. */
#define WAV_TARGET_PEAK 23197.0f
/* Below this much gain the recording is left as it is. */
#define WAV_MIN_GAIN    1.05f

static void le16(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
}

static void le32(uint8_t *p, uint32_t v)
{
    le16(p, v & 0xffff);
    le16(p + 2, v >> 16);
}

void wav_header(uint8_t *out, uint32_t rate, size_t n)
{
    const uint32_t data = (uint32_t)(n * 2);
    memcpy(out, "RIFF", 4);
    le32(out + 4, 36 + data);
    memcpy(out + 8, "WAVEfmt ", 8);
    le32(out + 16, 16);       /* fmt chunk size */
    le16(out + 20, 1);        /* PCM */
    le16(out + 22, 1);        /* mono */
    le32(out + 24, rate);
    le32(out + 28, rate * 2); /* bytes per second */
    le16(out + 32, 2);        /* bytes per frame */
    le16(out + 34, 16);       /* bits per sample */
    memcpy(out + 36, "data", 4);
    le32(out + 40, data);
}

float wav_normalize(int16_t *pcm, size_t n, float max_gain)
{
    int peak = 0;
    for (size_t i = 0; i < n; i++) {
        const int a = pcm[i] < 0 ? -(int)pcm[i] : pcm[i];
        if (a > peak) {
            peak = a;
        }
    }
    if (peak == 0) {
        return 1.0f;
    }
    float g = WAV_TARGET_PEAK / (float)peak;
    if (g > max_gain) {
        g = max_gain;
    }
    if (g < WAV_MIN_GAIN) {
        return 1.0f;
    }
    /* Fixed point (gain in 1/1024), so the loop stays integer. The peak
     * lands at or under the target, so nothing can clip. */
    const int32_t q = (int32_t)(g * 1024.0f);
    for (size_t i = 0; i < n; i++) {
        pcm[i] = (int16_t)(((int32_t)pcm[i] * q) / 1024);
    }
    return (float)q / 1024.0f;
}
