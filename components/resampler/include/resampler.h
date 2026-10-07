/*
 * Integer-ratio resampler for the telephone path.
 *
 * Decimator: low-pass FIR then keep every Nth sample (e.g. 48 kHz -> 8 kHz, N=6).
 * Interpolator: zero-stuff by N then the same low-pass (8 kHz -> 48 kHz).
 *
 * Windowed-sinc (Blackman) FIR designed at init, taps = 32*N + 1, so the
 * transition band is about 1.4 kHz wide regardless of N. Float math (the
 * ESP32-S3 has a single-precision FPU). Each instance keeps its own history,
 * so feed it a continuous stream in arbitrary chunk sizes.
 *
 * A ratio of 1 is a valid passthrough (copy, no filtering).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int ratio;      /* N */
    int taps;       /* FIR length */
    float *coef;    /* taps coefficients (decim) or taps coefficients scaled by N (interp) */
    float *hist;    /* 2*taps circular history (mirrored so a window is contiguous) */
    int pos;        /* next write index into hist, 0..taps-1 */
    int phase;      /* decimator: input samples since last output */
} resampler_t;

/*
 * Set up a decimator or interpolator.
 *   ratio:     integer factor N (>= 1)
 *   fs_high:   the high sample rate in Hz (input of decimator, output of interpolator)
 *   cutoff_hz: low-pass corner, e.g. 3500 for telephone band at 8 kHz
 * Returns false on allocation failure or bad arguments.
 */
bool resampler_decim_init(resampler_t *r, int ratio, float fs_high, float cutoff_hz);
bool resampler_interp_init(resampler_t *r, int ratio, float fs_high, float cutoff_hz);

/* Free buffers. Safe on a zeroed or already-freed struct. */
void resampler_deinit(resampler_t *r);

/* Clear history (call when the stream restarts). */
void resampler_reset(resampler_t *r);

/*
 * Decimate n_in samples. Writes up to (n_in + N - 1) / N samples to out.
 * Returns the number of output samples written.
 */
size_t resampler_decim_process(resampler_t *r, const int16_t *in, size_t n_in, int16_t *out);

/*
 * Interpolate n_in samples. Writes exactly n_in * N samples to out.
 * Returns the number of output samples written.
 */
size_t resampler_interp_process(resampler_t *r, const int16_t *in, size_t n_in, int16_t *out);

#ifdef __cplusplus
}
#endif
