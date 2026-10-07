/*
 * Audio engine: one FreeRTOS task (core 1) that moves APP_BLOCK_MS blocks.
 *
 *   USB mic ring (driver) -> unpack to mono int16 -> mic jitter ring
 *   mic jitter ring -> mode processing -> pack to device format -> USB earpiece ring (driver)
 *
 * The blocking write into the earpiece ring paces the loop to the device clock.
 */
#pragma once

void audio_engine_start(void);
