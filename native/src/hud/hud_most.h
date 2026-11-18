/* The HUD terminal (MOST FBlock 0x82/00) over the DSI client: open, send one call, collect answers.
 * Open order follows most_link.c: wait for the MOST client, register key 0x8200, wait for informationConnect.
 * Untested on a car: the key and the answer layout are inferred from hud-shape-qnx. */
#ifndef HUD_MOST_H
#define HUD_MOST_H

#include "hud_proto.h"

#include <stdint.h>

enum hud_most_rc { HUD_MOST_UP = 0, HUD_MOST_NO_TARGET = -1, HUD_MOST_NO_SERVER = -2, HUD_MOST_NO_LIBRARY = -3 };

typedef struct { uint16_t fkt; uint8_t op; uint8_t n; uint8_t p[32]; } hud_answer;

/* Which 82/xx terminal to open (default 0); call before hud_most_open. */
void hud_most_set_instance(uint8_t inst);
enum hud_most_rc hud_most_open(int timeout_ms);
/* hud_send_fn for hud_sender. */
int hud_most_send(void *u, const hud_call *c);
/* Takes up to max answers received since the last call. Payloads are cut to 32 bytes. */
int hud_most_poll(hud_answer *out, int max);
/* 0 while the client and the terminal are up. */
int hud_most_alive(void);
/* The device that hosts 82/00, as the registry reported it; 0 when not seen. */
uint16_t hud_most_host(void);
void hud_most_close(void);

#endif
