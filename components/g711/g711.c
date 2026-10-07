/*
 * G.711 mu-law, classic segment encoder (Sun reference algorithm).
 * The encoder works on 14-bit magnitude, so the 16-bit input is shifted
 * down by 2 first. Bias 0x84 (132) in the 16-bit domain, clip at 32635.
 */
#include "g711.h"

#define ULAW_BIAS 0x84
#define ULAW_CLIP 32635

uint8_t g711_ulaw_encode(int16_t pcm)
{
    int sample = pcm;
    int sign = (sample >> 8) & 0x80;
    if (sign) {
        sample = -sample;
    }
    if (sample > ULAW_CLIP) {
        sample = ULAW_CLIP;
    }
    sample += ULAW_BIAS;

    /* Exponent: position of the highest set bit above bit 7. */
    int exponent = 7;
    for (int mask = 0x4000; (sample & mask) == 0 && exponent > 0; mask >>= 1) {
        exponent--;
    }
    int mantissa = (sample >> (exponent + 3)) & 0x0F;
    return (uint8_t)~(sign | (exponent << 4) | mantissa);
}

int16_t g711_ulaw_decode(uint8_t ulaw)
{
    ulaw = (uint8_t)~ulaw;
    int sign = ulaw & 0x80;
    int exponent = (ulaw >> 4) & 0x07;
    int mantissa = ulaw & 0x0F;
    int sample = (((mantissa << 3) + ULAW_BIAS) << exponent) - ULAW_BIAS;
    return (int16_t)(sign ? -sample : sample);
}

void g711_ulaw_encode_buf(const int16_t *pcm, uint8_t *ulaw, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        ulaw[i] = g711_ulaw_encode(pcm[i]);
    }
}

void g711_ulaw_decode_buf(const uint8_t *ulaw, int16_t *pcm, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        pcm[i] = g711_ulaw_decode(ulaw[i]);
    }
}
