#include "hud_sender.h"

#include <string.h>

void hud_sender_init(hud_sender *s, hud_send_fn send, void *u)
{
    memset(s, 0, sizeof *s);
    s->send = send;
    s->u = u;
    s->window = HUD_IN_FLIGHT;
}

void hud_sender_set_window(hud_sender *s, unsigned n)
{
    s->window = n < 1 ? 1 : n > HUD_MAX_IN_FLIGHT ? HUD_MAX_IN_FLIGHT : n;
}

void hud_sender_set_pipeline(hud_sender *s, int on) { s->pipeline = on != 0; }

static void take_next(hud_sender *s, uint32_t now)
{
    s->sent = s->answered = 0;
    s->cur = s->next;
    s->have_next = 0;
    s->have_cur = 1;
    s->started = now;
    s->st.batches++;
}

static void start_next(hud_sender *s, uint32_t now)
{
    /* the old frame's calls leave the flight list either way; late answers count as strays */
    s->have_cur = s->have_tail = 0;
    s->sent = s->answered = 0;
    s->nflight = 0;
    if (!s->have_next) return;
    take_next(s, now);
}

/* Pipelining: cur is all sent, so it becomes the tail and the waiting batch starts. */
static void overlap(hud_sender *s, uint32_t now)
{
    unsigned i;
    for (i = 0; i < s->nflight; i++) s->flight[i].tail = 1;
    s->have_tail = 1;
    s->tail_n = s->cur.n;
    s->tail_answered = s->answered;
    s->tail_started = s->started;
    take_next(s, now);
}

static void done_ms(hud_sender *s, uint32_t ms)
{
    s->st.done++;
    s->st.last_batch_ms = ms;
    if (ms > s->st.max_batch_ms) s->st.max_batch_ms = ms;
}

void hud_sender_submit(hud_sender *s, const hud_batch *b, uint32_t now_ms)
{
    if (b->n == 0) return;
    if (s->have_next) s->st.replaced++;
    s->next = *b;
    s->have_next = 1;
    if (!s->have_cur && !s->have_tail) start_next(s, now_ms);
    else if (!s->have_cur) take_next(s, now_ms);   /* pipelining: the tail's calls stay in flight */
    hud_sender_tick(s, now_ms);
}

static void finish(hud_sender *s, uint32_t now, int failed)
{
    if (failed) {
        s->st.failed += 1u + (unsigned)s->have_tail;   /* a timeout drops both overlapping frames */
        start_next(s, now);
        return;
    }
    done_ms(s, now - s->started);
    if (!s->have_tail) {
        start_next(s, now);
        return;
    }
    /* cur done before the tail (answers out of order): keep the tail's calls in flight */
    s->have_cur = 0;
    s->sent = s->answered = 0;
    if (s->have_next) take_next(s, now);
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
    if ((!s->have_cur && !s->have_tail) || i == s->nflight) {
        s->st.strays++;
        return;
    }
    {
        int tail = s->flight[i].tail;
        memmove(&s->flight[i], &s->flight[i + 1], (s->nflight - i - 1) * sizeof s->flight[0]);
        s->nflight--;
        s->st.answers++;
        if (!ok) s->st.errors++;
        if (tail) {
            if (++s->tail_answered == s->tail_n) {
                s->have_tail = 0;
                done_ms(s, now_ms - s->tail_started);
                if (!s->have_cur) start_next(s, now_ms);
            }
        } else if (++s->answered == s->cur.n) finish(s, now_ms, 0);
    }
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
    if (s->pipeline && s->have_cur && !s->have_tail && s->have_next && s->sent == s->cur.n) overlap(s, now_ms);
    while (s->have_cur && s->sent < s->cur.n && s->nflight < s->window) {
        const hud_call *c = &s->cur.call[s->sent];
        if (s->send(s->u, c) != 0) {
            finish(s, now_ms, 1);
            continue;
        }
        s->flight[s->nflight].fkt = c->fkt;
        s->flight[s->nflight].handle = hud_call_handle(c);
        s->flight[s->nflight].at = now_ms;
        s->flight[s->nflight].tail = 0;
        s->nflight++;
        s->sent++;
        s->st.calls++;
    }
}

int hud_sender_idle(const hud_sender *s) { return !s->have_cur && !s->have_tail && !s->have_next; }

int hud_sender_ready(const hud_sender *s)
{
    if (s->have_next) return 0;
    if (!s->have_cur) return !s->have_tail || s->pipeline;
    return s->pipeline && !s->have_tail && s->sent == s->cur.n;
}
