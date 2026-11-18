/* Sends a batch call by call, at most HUD_IN_FLIGHT awaiting an answer, and matches answers by
 * (function, handle). Transport-free: the caller gives it a send function and feeds answers and the clock.
 * Single-threaded; the MOST binding serializes access. */
#ifndef HUD_SENDER_H
#define HUD_SENDER_H

#include "hud_proto.h"

#include <stdint.h>

enum { HUD_ANSWER_TIMEOUT_MS = 1500 };

typedef int (*hud_send_fn)(void *u, const hud_call *c);   /* 0 when handed to the transport */

typedef struct {
    unsigned batches, done, replaced, failed;
    unsigned calls, answers, errors, timeouts, strays;
    uint32_t last_batch_ms, max_batch_ms;
} hud_sender_stats;

typedef struct {
    hud_send_fn send;
    void *u;
    hud_batch cur, next;
    int have_cur, have_next;
    unsigned sent;                   /* calls of cur handed to the transport */
    unsigned answered;
    struct { uint16_t fkt, handle; uint32_t at; } flight[HUD_IN_FLIGHT];
    unsigned nflight;
    uint32_t started;
    hud_sender_stats st;
} hud_sender;

void hud_sender_init(hud_sender *s, hud_send_fn send, void *u);
/* Queues a batch. While one is in progress the newest waiting batch replaces the older one, so a slow
   HUD shows the latest frame instead of falling behind. */
void hud_sender_submit(hud_sender *s, const hud_batch *b, uint32_t now_ms);
/* An answer from the terminal. op 0xC (Result) counts as done, 0x9/0xF (error) as done with an error. */
void hud_sender_answer(hud_sender *s, uint16_t fkt, uint8_t op, const uint8_t *p, unsigned n, uint32_t now_ms);
/* Sends what the window allows and expires calls older than HUD_ANSWER_TIMEOUT_MS. */
void hud_sender_tick(hud_sender *s, uint32_t now_ms);
int hud_sender_idle(const hud_sender *s);

#endif
