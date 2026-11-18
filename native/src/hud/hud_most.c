/* Answers arrive on the DSI thread and wait in a small ring under g_mu until the main thread polls. */
#define _POSIX_C_SOURCE 200809L
#include "hud_most.h"

#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "most/dsi_abi.h"
#include "most/dsi_client.h"

#define HUD_ROLE  "MOSTSubSystem"
#define RING      64

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_thr;
static int g_thr_running;
static int g_have_client, g_have_target, g_exited;
static uint16_t g_host;
static uint8_t g_inst = HUD_INST;   /* 82/00; 82/02 may be the HUD on a 6WB car (JayJay3071/nbt-android-auto) */
static hud_answer g_ring[RING];
static unsigned g_head, g_tail;

static void h_connected(void *u) { (void)u; }

static void h_disconnected(void *u)
{
    (void)u;
    pthread_mutex_lock(&g_mu);
    g_have_client = g_have_target = 0;
    pthread_mutex_unlock(&g_mu);
}

static void h_most_client(void *u, uint32_t client_id, uint16_t device_id)
{
    (void)u; (void)client_id; (void)device_id;
    pthread_mutex_lock(&g_mu);
    g_have_client = 1;
    pthread_mutex_unlock(&g_mu);
}

static void h_registry(void *u, const uint16_t *pairs, int n_pairs)
{
    int i;
    (void)u;
    for (i = 0; i < n_pairs; i++)
        if (pairs[2 * i + 1] == (uint16_t)(HUD_FBLOCK << 8 | g_inst)) {
            pthread_mutex_lock(&g_mu);
            g_host = pairs[2 * i];
            pthread_mutex_unlock(&g_mu);
        }
}

static void h_target_up(void *u, uint16_t device)
{
    (void)u;
    pthread_mutex_lock(&g_mu);
    g_have_target = 1;
    if (!g_host) g_host = device;
    pthread_mutex_unlock(&g_mu);
}

static void h_target_down(void *u)
{
    (void)u;
    pthread_mutex_lock(&g_mu);
    g_have_target = 0;
    pthread_mutex_unlock(&g_mu);
}

static void h_report(void *u, uint16_t fkt, uint8_t op, const uint8_t *p, size_t n)
{
    hud_answer *a;
    (void)u;
    pthread_mutex_lock(&g_mu);
    if (g_head - g_tail < RING) {
        a = &g_ring[g_head++ % RING];
        a->fkt = fkt;
        a->op = op;
        a->n = (uint8_t)(n < sizeof a->p ? n : sizeof a->p);
        memcpy(a->p, p, a->n);
    }
    pthread_mutex_unlock(&g_mu);
}

static void *run(void *arg)
{
    static const dsi_handlers_t h = {
        h_connected, h_disconnected, h_most_client, h_registry, h_target_up, h_target_down, h_report
    };
    sigset_t set;
    (void)arg;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
    dsi_client_run(HUD_ROLE, &h, NULL);
    pthread_mutex_lock(&g_mu);
    g_exited = 1;
    g_have_client = g_have_target = 0;
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

/* usleep is not in POSIX 2008, and QNX hides it under _POSIX_C_SOURCE 200809L. */
static void sleep_us(long us)
{
    struct timespec ts = { us / 1000000, us % 1000000 * 1000 };
    nanosleep(&ts, NULL);
}

/* Polls flag every 10 ms for up to ms. */
static int wait_flag(const int *flag, int ms)
{
    int v = 0, waited;
    for (waited = 0; waited <= ms; waited += 10) {
        pthread_mutex_lock(&g_mu);
        v = *flag && !g_exited;
        pthread_mutex_unlock(&g_mu);
        if (v) break;
        sleep_us(10000);
    }
    return v;
}

void hud_most_set_instance(uint8_t inst) { g_inst = inst; }

enum hud_most_rc hud_most_open(int timeout_ms)
{
    if (g_thr_running) return HUD_MOST_NO_SERVER;
    if (most_dsi_bind() != 0) return HUD_MOST_NO_LIBRARY;
    g_have_client = g_have_target = g_exited = 0;
    g_host = 0;
    g_head = g_tail = 0;
    dsi_client_set_name("hudsportbar");
    dsi_client_set_target((uint16_t)(HUD_FBLOCK << 8 | g_inst), HUD_FBLOCK, g_inst);
    if (pthread_create(&g_thr, NULL, run, NULL) != 0) return HUD_MOST_NO_SERVER;
    g_thr_running = 1;
    if (!wait_flag(&g_have_client, timeout_ms)) return HUD_MOST_NO_SERVER;
    dsi_register_target();
    return wait_flag(&g_have_target, timeout_ms) ? HUD_MOST_UP : HUD_MOST_NO_TARGET;
}

int hud_most_send(void *u, const hud_call *c)
{
    (void)u;
    return dsi_send_fn(HUD_FBLOCK, g_inst, c->fkt, HUD_OPTYPE, c->p, c->len);
}

int hud_most_poll(hud_answer *out, int max)
{
    int n = 0;
    pthread_mutex_lock(&g_mu);
    while (n < max && g_tail != g_head) out[n++] = g_ring[g_tail++ % RING];
    pthread_mutex_unlock(&g_mu);
    return n;
}

int hud_most_alive(void)
{
    int up;
    pthread_mutex_lock(&g_mu);
    up = g_have_client && g_have_target && !g_exited;
    pthread_mutex_unlock(&g_mu);
    return up ? 0 : -1;
}

uint16_t hud_most_host(void)
{
    uint16_t h;
    pthread_mutex_lock(&g_mu);
    h = g_host;
    pthread_mutex_unlock(&g_mu);
    return h;
}

void hud_most_close(void)
{
    int waited, exited = 0;
    if (!g_thr_running) return;
    dsi_unregister_target();
    dsi_unregister_client();
    for (waited = 0; waited < 3000 && !exited; waited += 50) {
        dsi_client_stop();
        sleep_us(50000);
        pthread_mutex_lock(&g_mu);
        exited = g_exited;
        pthread_mutex_unlock(&g_mu);
    }
    if (exited) pthread_join(g_thr, NULL);
    g_thr_running = 0;
}
