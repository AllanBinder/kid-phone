#include "resampler.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static inline int16_t clamp16(float v)
{
    if (v > 32767.0f) {
        return 32767;
    }
    if (v < -32768.0f) {
        return -32768;
    }
    return (int16_t)lrintf(v);
}

/* Blackman-windowed sinc low-pass, unity DC gain, then scaled by gain. */
static void design_lowpass(float *h, int taps, float fs, float fc, float gain)
{
    const float wc = fc / fs; /* normalized, cycles per sample */
    const int m = taps - 1;
    double sum = 0.0;
    for (int i = 0; i < taps; i++) {
        double x = i - m / 2.0;
        double s = (x == 0.0) ? 2.0 * wc : sin(2.0 * M_PI * wc * x) / (M_PI * x);
        double w = 0.42 - 0.5 * cos(2.0 * M_PI * i / m) + 0.08 * cos(4.0 * M_PI * i / m);
        h[i] = (float)(s * w);
        sum += h[i];
    }
    for (int i = 0; i < taps; i++) {
        h[i] = (float)(h[i] / sum * gain);
    }
}

static bool common_init(resampler_t *r, int ratio, float fs_high, float cutoff_hz, float gain)
{
    memset(r, 0, sizeof(*r));
    if (ratio < 1 || fs_high <= 0.0f || cutoff_hz <= 0.0f || cutoff_hz >= fs_high / 2.0f) {
        return false;
    }
    r->ratio = ratio;
    if (ratio == 1) {
        return true; /* passthrough, no buffers */
    }
    r->taps = 32 * ratio + 1;
    r->coef = calloc(r->taps, sizeof(float));
    r->hist = calloc(2 * r->taps, sizeof(float));
    if (!r->coef || !r->hist) {
        resampler_deinit(r);
        return false;
    }
    design_lowpass(r->coef, r->taps, fs_high, cutoff_hz, gain);
    return true;
}

bool resampler_decim_init(resampler_t *r, int ratio, float fs_high, float cutoff_hz)
{
    return common_init(r, ratio, fs_high, cutoff_hz, 1.0f);
}

bool resampler_interp_init(resampler_t *r, int ratio, float fs_high, float cutoff_hz)
{
    /* Zero-stuffing divides the signal energy by N, so the filter gains N. */
    return common_init(r, ratio, fs_high, cutoff_hz, (float)ratio);
}

void resampler_deinit(resampler_t *r)
{
    free(r->coef);
    free(r->hist);
    r->coef = NULL;
    r->hist = NULL;
}

void resampler_reset(resampler_t *r)
{
    if (r->hist) {
        memset(r->hist, 0, 2 * r->taps * sizeof(float));
    }
    r->pos = 0;
    r->phase = 0;
}

/* Push one sample into the mirrored history. */
static inline void push(resampler_t *r, float v)
{
    r->hist[r->pos] = v;
    r->hist[r->pos + r->taps] = v;
    if (++r->pos == r->taps) {
        r->pos = 0;
    }
}

/*
 * After a push, hist[pos .. pos+taps-1] holds the last `taps` samples,
 * oldest first. coef is symmetric, so ordering of the dot product is moot.
 */
static inline float dot(const resampler_t *r)
{
    const float *x = &r->hist[r->pos];
    const float *h = r->coef;
    float acc = 0.0f;
    for (int i = 0; i < r->taps; i++) {
        acc += x[i] * h[i];
    }
    return acc;
}

size_t resampler_decim_process(resampler_t *r, const int16_t *in, size_t n_in, int16_t *out)
{
    if (r->ratio == 1) {
        memcpy(out, in, n_in * sizeof(int16_t));
        return n_in;
    }
    size_t n_out = 0;
    for (size_t i = 0; i < n_in; i++) {
        push(r, (float)in[i]);
        if (++r->phase == r->ratio) {
            r->phase = 0;
            out[n_out++] = clamp16(dot(r));
        }
    }
    return n_out;
}

size_t resampler_interp_process(resampler_t *r, const int16_t *in, size_t n_in, int16_t *out)
{
    if (r->ratio == 1) {
        memcpy(out, in, n_in * sizeof(int16_t));
        return n_in;
    }
    /*
     * Straightforward form: push the sample then N-1 zeros, filtering after
     * each push. At 8 kHz in and N=6 that is 48k * 193 MACs per second,
     * about 9 M MAC/s, a few percent of one core. A polyphase rewrite can
     * skip the zero products later if the SIP stage needs the headroom.
     */
    size_t n_out = 0;
    for (size_t i = 0; i < n_in; i++) {
        push(r, (float)in[i]);
        out[n_out++] = clamp16(dot(r));
        for (int k = 1; k < r->ratio; k++) {
            push(r, 0.0f);
            out[n_out++] = clamp16(dot(r));
        }
    }
    return n_out;
}
