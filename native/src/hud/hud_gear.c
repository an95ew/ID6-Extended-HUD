#include "hud_gear.h"

#include "drive_frame.h"

#include <stddef.h>

typedef struct { int source, raw, shown; } row;

/* EGS 0x18, UDS 22 DA 2E: raw = reply byte 3 << 8 | byte 4.
   [car] 00 06 = P: ID6_apps' Park check (BEAMer, controls) on this car. D/S gears: car run 6, step G1. */
static const row k_gear[] = {
    { DRIVE_SRC_OBD, 0x0006, 0 },   /* P: hidden */
    /* CDS driving.gear [car] run 6 C1, standing, engine on, the owner's P -> R -> N -> D -> S: */
    { DRIVE_SRC_CDS, 3, 0 },        /* P */
    { DRIVE_SRC_CDS, 2, 0 },        /* R */
    { DRIVE_SRC_CDS, 1, 0 },        /* N */
    /* D/S/M: the gear counter, value - 4. [car] 5 = 1st, 6 = 2nd (run 6, pulling away and in the yard; S standing = 5 too).
       7..12 = 3rd..8th [inferred: the same counter], until a drive shows them. */
    { DRIVE_SRC_CDS, 5, 1 }, { DRIVE_SRC_CDS, 6, 2 }, { DRIVE_SRC_CDS, 7, 3 }, { DRIVE_SRC_CDS, 8, 4 },
    { DRIVE_SRC_CDS, 9, 5 }, { DRIVE_SRC_CDS, 10, 6 }, { DRIVE_SRC_CDS, 11, 7 }, { DRIVE_SRC_CDS, 12, 8 },
};

/* CDS driving.mode [car] run 6 C1: the owner's Comfort -> Sport -> Sport+ -> Eco Pro -> Comfort gave 3, 4, 5, (4, 3,) 7, 3. */
static const row k_mode[] = {
    { DRIVE_SRC_CDS, 3, HUD_MODE_COMFORT },
    { DRIVE_SRC_CDS, 4, HUD_MODE_SPORT },
    { DRIVE_SRC_CDS, 5, HUD_MODE_SPORT_PLUS },
    { DRIVE_SRC_CDS, 7, HUD_MODE_ECO_PRO },
};

static int lookup(const row *t, size_t n, int source, int raw, int none)
{
    size_t i;
    if (raw < 0) return none;
    for (i = 0; i < n; i++)
        if (t[i].source == source && t[i].raw == raw) return t[i].shown;
    return none;
}

int hud_gear_decode(int source, int raw) { return lookup(k_gear, sizeof k_gear / sizeof k_gear[0], source, raw, 0); }

hud_drive_mode hud_mode_decode(int source, int raw)
{
    return (hud_drive_mode)lookup(k_mode, sizeof k_mode / sizeof k_mode[0], source, raw, HUD_MODE_UNKNOWN);
}

void hud_live_init(hud_live *l)
{
    l->rpm = l->gear_raw = l->mode_raw = -1;
    l->gear_src = l->mode_src = 0;
    l->rpm_ms = l->gear_ms = l->mode_ms = 0;
    l->cds_rpm_ms = l->cds_gear_ms = 0;
}

static int fresh(uint32_t at, uint32_t now, uint32_t ms) { return at && now - at < ms; }

int hud_live_feed(hud_live *l, const struct drive_frame *f, uint32_t now_ms)
{
    int changed = 0;
    if (!now_ms) now_ms = 1;
    int cds = f->source == DRIVE_SRC_CDS;
    if (f->rpm >= 0 && (cds || !fresh(l->cds_rpm_ms, now_ms, HUD_DRIVE_STALE_MS))) {
        l->rpm = f->rpm;
        l->rpm_ms = now_ms;
        if (cds) l->cds_rpm_ms = now_ms;
    }
    if (f->gear_raw >= 0 && (cds || !fresh(l->cds_gear_ms, now_ms, HUD_DRIVE_STALE_MS))) {
        l->gear_raw = f->gear_raw;
        l->gear_src = f->source;
        l->gear_ms = now_ms;
        if (cds) l->cds_gear_ms = now_ms;
    }
    if (f->mode_raw >= 0) {
        changed = f->mode_raw != l->mode_raw || f->source != l->mode_src;
        l->mode_raw = f->mode_raw;
        l->mode_src = f->source;
        l->mode_ms = now_ms;
    }
    return changed;
}

int hud_live_rpm(const hud_live *l, uint32_t now_ms) { return fresh(l->rpm_ms, now_ms, HUD_DRIVE_STALE_MS) ? l->rpm : -1; }

int hud_live_gear(const hud_live *l, uint32_t now_ms)
{
    return fresh(l->gear_ms, now_ms, HUD_DRIVE_STALE_MS) ? hud_gear_decode(l->gear_src, l->gear_raw) : 0;
}

hud_drive_mode hud_live_mode(const hud_live *l, uint32_t now_ms)
{
    return fresh(l->mode_ms, now_ms, HUD_MODE_STALE_MS) ? hud_mode_decode(l->mode_src, l->mode_raw) : HUD_MODE_UNKNOWN;
}

hud_scene hud_live_gate(hud_scene scene, int rpm, int gear)
{
    return scene == HUD_SCENE_RPM && !(rpm > 0 && gear >= 1 && gear <= 9) ? HUD_SCENE_NONE : scene;
}
