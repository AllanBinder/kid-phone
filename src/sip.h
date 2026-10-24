/*
 * Minimal SIP user agent over UDP: outbound calls only, no REGISTER.
 *
 * One task (core 0) owns the SIP socket, the call state machine and all
 * timers. Other tasks talk to it through sip_call_contact() / sip_call_hangup()
 * (queued commands) and read the state with sip_call_state().
 *
 *   IDLE --start--> CALLING --1xx--> RINGING --200--> IN_CALL
 *     ^                |                |                |
 *     |              4xx-6xx,         hangup:          hangup: BYE,
 *     |              timeout          CANCEL           or remote BYE
 *     |                |                |                |
 *     +----------------+---- ENDING <---+----------------+
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CALL_IDLE = 0,
    CALL_CALLING,  /* INVITE sent, no provisional response yet */
    CALL_RINGING,  /* 180/183 received (early media plays if the 183 had SDP) */
    CALL_IN_CALL,  /* 200 OK received and ACKed */
    CALL_ENDING,   /* CANCEL or BYE sent, waiting for the transaction to finish */
} call_state_t;

/* Bind the SIP socket and start the SIP task. Call after wifi_start(). */
void sip_start(void);

/* Queue a call to contact idx (0..CFG_MAX_CONTACTS-1). Ignored unless idle. */
void sip_call_contact(int idx);

/* Queue a hang up: CANCEL while ringing, BYE in a call. */
void sip_call_hangup(void);

call_state_t sip_call_state(void);

/* Contact index of the current call attempt, -1 when idle. */
int sip_call_contact_index(void);

/* Outcome of the most recent call attempt (seq bumps on every new result). */
typedef struct {
    uint32_t seq;
    bool failed;          /* never connected: SIP error final, no response, DNS, no WiFi, not set up */
    int code;             /* SIP status of the failing final response, 0 if none */
    bool answered;
    bool remote_hangup;   /* the far end (or Twilio) sent the BYE */
    uint32_t talk_ms;     /* answer to hang up */
    int contact;          /* contact index of the call */
    char reason[64];      /* technical, for logs */
    /* Plain-language explanation for the setup page, "" if the call went fine:
     *   kind "login"       403 after we sent credentials, or 401/407 twice
     *   kind "domain"      404 (SIP domain unknown)
     *   kind "unreachable" Twilio answered, then hung up within APP_SHORT_CALL_MS
     *   kind "busy"        480 / 486 / 600 / 603
     *   kind "network"     DNS, no WiFi, no response
     *   kind "setup"       not set up, or no number on that button
     *   kind "timeup"      the phone hung up at the call length limit
     *   kind "noanswer"    the phone gave up ringing (APP_SIP_RING_TIMEOUT_MS)
     *   kind "other"       any other failure */
    char kind[12];
    char message[160];
} sip_result_t;

void sip_last_result(sip_result_t *out);

/* seq of the latest result (cheap to poll; main reacts to a change). */
uint32_t sip_result_seq(void);
const char *sip_call_state_name(call_state_t s);
