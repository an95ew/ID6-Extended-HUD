#include "hud_sender.h"

#include <string.h>

void hud_sender_init(hud_sender *s, hud_send_fn send, void *u)
{
    memset(s, 0, sizeof *s);
    s->send = send;
    s->u = u;
}

static void start_next(hud_sender *s, uint32_t now)
{
    /* the old frame's calls leave the flight list either way; late answers count as strays */
    s->have_cur = 0;
    s->sent = s->answered = 0;
    s->nflight = 0;
    if (!s->have_next) return;
    s->cur = s->next;
    s->have_next = 0;
    s->have_cur = 1;
    s->started = now;
    s->st.batches++;
}

void hud_sender_submit(hud_sender *s, const hud_batch *b, uint32_t now_ms)
{
    if (b->n == 0) return;
    if (s->have_next) s->st.replaced++;
    s->next = *b;
    s->have_next = 1;
    if (!s->have_cur) start_next(s, now_ms);
    hud_sender_tick(s, now_ms);
}

static void finish(hud_sender *s, uint32_t now, int failed)
{
    uint32_t ms = now - s->started;
    if (failed) s->st.failed++;
    else {
        s->st.done++;
        s->st.last_batch_ms = ms;
        if (ms > s->st.max_batch_ms) s->st.max_batch_ms = ms;
    }
    start_next(s, now);
}

void hud_sender_answer(hud_sender *s, uint16_t fkt, uint8_t op, const uint8_t *p, unsigned n, uint32_t now_ms)
{
    uint16_t h = n >= 2 ? (uint16_t)(p[0] << 8 | p[1]) : 0;
    unsigned i;
    /* OpType 6 (StartResultAck) is answered with ResultAck D (what the 6WB sends) or ErrorAck 9; C and F are the
       plain forms. ProcessingAck A and the others are only notes. */
    int ok = op == 0xC || op == 0xD;
    if (!ok && op != 0x9 && op != 0xF) return;
    for (i = 0; i < s->nflight; i++)
        if (s->flight[i].fkt == fkt && s->flight[i].handle == h) break;
    if (!s->have_cur || i == s->nflight) {
        s->st.strays++;
        return;
    }
    memmove(&s->flight[i], &s->flight[i + 1], (s->nflight - i - 1) * sizeof s->flight[0]);
    s->nflight--;
    s->answered++;
    s->st.answers++;
    if (!ok) s->st.errors++;
    if (s->answered == s->cur.n) finish(s, now_ms, 0);
    hud_sender_tick(s, now_ms);
}

void hud_sender_tick(hud_sender *s, uint32_t now_ms)
{
    unsigned i;
    for (i = 0; i < s->nflight; i++)
        if (now_ms - s->flight[i].at > HUD_ANSWER_TIMEOUT_MS) {
            s->st.timeouts++;
            finish(s, now_ms, 1);   /* drop the rest of the frame; the next one redraws everything */
            break;
        }
    while (s->have_cur && s->sent < s->cur.n && s->nflight < HUD_IN_FLIGHT) {
        const hud_call *c = &s->cur.call[s->sent];
        if (s->send(s->u, c) != 0) {
            finish(s, now_ms, 1);
            continue;
        }
        s->flight[s->nflight].fkt = c->fkt;
        s->flight[s->nflight].handle = hud_call_handle(c);
        s->flight[s->nflight].at = now_ms;
        s->nflight++;
        s->sent++;
        s->st.calls++;
    }
}

int hud_sender_idle(const hud_sender *s) { return !s->have_cur && !s->have_next; }
