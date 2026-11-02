/*
 * Audio engine: one FreeRTOS task (core 1) that moves APP_BLOCK_MS blocks.
 *
 *   USB mic ring (driver) -> unpack to mono int16 -> mic jitter ring
 *   mic jitter ring -> mode processing -> pack to device format -> USB earpiece ring (driver)
 *
 * The blocking write into the earpiece ring paces the loop to the device clock.
 *
 * Spoken prompts (prompts.h) are 16 kHz clips in flash, upsampled block by
 * block with their own interpolator, so they never stall the loop.
 *
 * Secret messages (secret.h): MODE_SECRET hands the mic ring to the
 * recorder instead of the earpiece, and MODE_BEEP is its beep.
 */
#pragma once

void audio_engine_start(void);
