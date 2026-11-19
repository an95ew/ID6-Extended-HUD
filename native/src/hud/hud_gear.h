/* What the car's raw values mean for the HUD (struct drive_frame gear_raw, mode_raw). Actual data only (owner,
 * 2026-11-18): the gear number in D/S, nothing in P, R and N, and nothing for a raw value not in the maps, never a
 * guess from RPM and speed. A row goes into a map only once it has been seen on the car. */
#ifndef HUD_GEAR_H
#define HUD_GEAR_H

#include "hud_scene.h"

#include <stdint.h>

/* hud_anim.gear for a raw value: 1..9 in D/S, 0 = hidden (P, R, N, unknown). source is DRIVE_SRC_*, raw -1 = not read. */
int hud_gear_decode(int source, int raw);
/* The driving mode for a raw value; HUD_MODE_UNKNOWN (the scene treats it as Comfort) when not in the map. */
hud_drive_mode hud_mode_decode(int source, int raw);

/* The newest of each value, from whichever source sent it (the collector over OBD, the plugin over CDS). */
struct drive_frame;
typedef struct {
    int rpm, gear_raw, gear_src, mode_raw, mode_src;
    uint32_t rpm_ms, gear_ms, mode_ms;   /* when it arrived, 0 = never */
    uint32_t cds_rpm_ms, cds_gear_ms;    /* the last CDS revs / gear: while fresh, the collector's are ignored */
} hud_live;

/* Over the MOST tunnel one request takes ~260 ms (car 2026-11-18), and the collector takes turns between the revs and
   the gear, so a value is up to ~0.6 s old when it is fresh. Older than this the bar shows its dim track only: no lit
   fields, gear or number from old data. A driving mode lasts until its source goes quiet. */
enum { HUD_DRIVE_STALE_MS = 2000, HUD_MODE_STALE_MS = 5000 };

void hud_live_init(hud_live *l);
/* Takes the frame's fields that are not -1; while CDS sends revs or the gear (every ~100 ms, car run 6), OBD's of the
   same kind are ignored, so the two never mix. Returns 1 when the driving mode's raw value or source changed. */
int hud_live_feed(hud_live *l, const struct drive_frame *f, uint32_t now_ms);
/* The revs when fresh, else -1. */
int hud_live_rpm(const hud_live *l, uint32_t now_ms);
/* hud_gear_decode of the fresh gear, else 0 (hidden). */
int hud_live_gear(const hud_live *l, uint32_t now_ms);
hud_drive_mode hud_live_mode(const hud_live *l, uint32_t now_ms);
/* Owner 2026-11-18: the Sport Bar only with the engine running (rpm > 0, fresh) and the gearbox in D, S or M (a mapped gear
   1..9). Otherwise the scene becomes NONE: engine off, P, N, R, an unmapped gearbox value, no fresh data. */
hud_scene hud_live_gate(hud_scene scene, int rpm, int gear);

#endif
