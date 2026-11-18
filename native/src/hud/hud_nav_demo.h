/* A scripted route through typical Ukrainian lane situations, for the desk preview and the parked car test.
 * The streets are examples; the lane layouts are illustrative, not surveyed. */
#ifndef HUD_NAV_DEMO_H
#define HUD_NAV_DEMO_H

#include "hud_nav.h"

#include <stdint.h>

/* "ua" (default): Ukrainian lane situations. "intl": other languages and hostile text. -1 when unknown. */
int hud_nav_demo_use(const char *name);
/* The route position at t ms (it loops). Each manoeuvre counts down in 8 s, then holds 1 s at 0 m. */
void hud_nav_demo_at(uint32_t t_ms, hud_nav_info *out);
uint32_t hud_nav_demo_length_ms(void);
int hud_nav_demo_count(void);
/* Manoeuvre i at distance d, for tests. */
void hud_nav_demo_get(int i, int dist_m, hud_nav_info *out);

#endif
