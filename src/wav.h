/*
 * The secret message recording as a WAV file: 16-bit mono PCM behind a
 * 44 byte header, and a gentle level boost before it is sent (toddlers
 * whisper secrets). Pure C, no ESP-IDF dependency (host-testable).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define WAV_HEADER_BYTES 44

/* Write the canonical 44 byte header for n mono 16-bit samples at rate Hz. */
void wav_header(uint8_t *out, uint32_t rate, size_t n);

/* Raise the level so the loudest sample reaches about -3 dBFS, by at most
 * max_gain (a whisper is boosted, a shout is left alone; never turned down).
 * Returns the gain applied (1.0 = unchanged). */
float wav_normalize(int16_t *pcm, size_t n, float max_gain);
