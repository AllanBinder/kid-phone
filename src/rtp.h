/*
 * RTP media for one call: PCMU (payload type 0), 8 kHz, 20 ms frames of
 * 160 bytes, one UDP socket used for both directions (symmetric RTP).
 *
 * Audio side (audio task, core 1):
 *   rtp_audio_tx_push()  mu-law bytes from the mic path, any chunk size
 *   rtp_audio_rx_pull()  mu-law bytes for the earpiece, from the jitter buffer
 * Network side (two tasks on core 0): the sender drains a stream buffer in
 * 160-byte frames, the receiver fills the jitter buffer.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RTP_FRAME_BYTES 160 /* 20 ms of PCMU at 8 kHz */

/* Bind the media socket and start the sender and receiver tasks. */
bool rtp_start(void);

/* Local media port (for the SDP m= line). */
uint16_t rtp_local_port(void);

/* New call: fresh SSRC, sequence and timestamp, cleared jitter buffer and
 * stats. Incoming packets are accepted from now on; nothing is sent until
 * rtp_set_remote(). */
void rtp_session_begin(void);

/* Remote media address from the SDP (dotted IPv4). Enables sending. May be
 * called again (183 then 200) with the same or a new address. */
void rtp_set_remote(const char *ip, uint16_t port);

/* Stop sending and receiving, log final stats. */
void rtp_session_end(void);

/* Audio task side. Push never blocks (drops when full). Pull always fills
 * n bytes, with mu-law silence (0xFF) when nothing is playable. */
void rtp_audio_tx_push(const uint8_t *ulaw, size_t n);
void rtp_audio_rx_pull(uint8_t *ulaw, size_t n);

/* True once any PCMU packet has arrived in the current session (early media
 * or answer). Before that the earpiece plays local ringback while ringing. */
bool rtp_media_received(void);
