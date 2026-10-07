/*
 * G.711 mu-law codec (ITU-T G.711, North American / Japanese variant).
 *
 * This is the payload format for RTP payload type 0 (PCMU), 8 kHz mono,
 * which is what the Twilio SIP stage will carry. Pure functions, no state,
 * safe to call from any task.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Encode one signed 16-bit linear PCM sample to an 8-bit mu-law byte. */
uint8_t g711_ulaw_encode(int16_t pcm);

/* Decode one 8-bit mu-law byte to a signed 16-bit linear PCM sample. */
int16_t g711_ulaw_decode(uint8_t ulaw);

/* Buffer helpers: n samples in, n bytes out (and the reverse). */
void g711_ulaw_encode_buf(const int16_t *pcm, uint8_t *ulaw, size_t n);
void g711_ulaw_decode_buf(const uint8_t *ulaw, int16_t *pcm, size_t n);

#ifdef __cplusplus
}
#endif
