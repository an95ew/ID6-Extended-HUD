/* Required Notice: Copyright 2026 nbtevo-carplay-advanced contributors
 * Licensed under the PolyForm Noncommercial License 1.0.0 (https://polyformproject.org/licenses/noncommercial/1.0.0)
 *
 * The routeguidance file -> hud_nav_info. The reader follows readGuide() in st1ng's src/hud.h; the maneuver
 * table follows their shapeOf() (src/hudshapes.h) mapped onto our hud_maneuver. */
#define _POSIX_C_SOURCE 200809L
#include "hud_rg.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* "key:type:value": 1 when the line's key is k; *v gets the value (after the second colon). */
static int key_is(const char *l, const char *e, const char *k, const char **v)
{
    size_t n = strlen(k);
    const char *c;
    if ((size_t)(e - l) <= n || memcmp(l, k, n) || l[n] != ':') return 0;
    c = l + n + 1;
    while (c < e && *c != ':') c++;
    *v = c < e ? c + 1 : e;
    return 1;
}

/* A decimal with an optional "-"; *p moves past it. *ok = a digit was there. */
static int num(const char **p, const char *e, int *ok)
{
    const char *q = *p;
    int neg = 0;
    long v = 0;
    *ok = 0;
    if (q < e && *q == '-') { neg = 1; q++; }
    while (q < e && *q >= '0' && *q <= '9') {
        if (v < 100000000) v = v * 10 + (*q - '0');
        q++;
        *ok = 1;
    }
    *p = q;
    return (int)(neg ? -v : v);
}

static int num_of(const char *v, const char *e)
{
    int ok;
    return num(&v, e, &ok);
}

static void copy_val(char *d, size_t cap, const char *v, const char *e)
{
    size_t n = (size_t)(e - v);
    if (n >= cap) n = cap - 1;
    memcpy(d, v, n);
    d[n] = 0;
}

int hud_rg_roundabout_angle(int angle)
{
    int a;
    if (!angle) return 0;
    while (angle > 180) angle -= 360;
    while (angle < -180) angle += 360;
    a = 180 - angle;                /* right (+90) = 90, straight (0) = 180, left (-90) = 270, back = 360 */
    return a <= 0 ? a + 360 : a;
}

/* A turn by its angle alone, as st1ng's byAngle(): straight, slight, turn, sharp, U-turn. */
static enum hud_maneuver by_angle(int a)
{
    int m = a < 0 ? -a : a, left = a < 0;
    if (m <= 20) return HUD_MAN_STRAIGHT;
    if (m <= 65) return left ? HUD_MAN_SLIGHT_LEFT : HUD_MAN_SLIGHT_RIGHT;
    if (m <= 125) return left ? HUD_MAN_LEFT : HUD_MAN_RIGHT;
    if (m <= 165) return left ? HUD_MAN_SHARP_LEFT : HUD_MAN_SHARP_RIGHT;
    return HUD_MAN_UTURN;
}

enum hud_maneuver hud_rg_maneuver(unsigned type, int angle, int side, int *exit)
{
    if (exit) *exit = type >= 28 && type <= 46 ? (int)type - 27 : 0;
    switch (type) {
    case 1: case 20: return HUD_MAN_LEFT;                 /* left, left at the end of the road */
    case 2: case 21: return HUD_MAN_RIGHT;
    case 47: return HUD_MAN_SHARP_LEFT;
    case 48: return HUD_MAN_SHARP_RIGHT;
    case 49: return HUD_MAN_SLIGHT_LEFT;
    case 50: return HUD_MAN_SLIGHT_RIGHT;
    case 51: return HUD_MAN_STRAIGHT;                     /* change highway */
    case 13: case 52: return HUD_MAN_KEEP_LEFT;           /* keep left, change highway left */
    case 14: case 53: return HUD_MAN_KEEP_RIGHT;
    case 22: return HUD_MAN_EXIT_LEFT;                    /* highway off-ramp left */
    case 23: return HUD_MAN_EXIT_RIGHT;
    case 8: return (angle ? angle < 0 : side == 1) ? HUD_MAN_EXIT_LEFT : HUD_MAN_EXIT_RIGHT;   /* off-ramp */
    case 9: return HUD_MAN_MERGE;                         /* on-ramp */
    case 4: case 18: case 26: return HUD_MAN_UTURN;       /* U-turn, start with a U-turn, U-turn when possible */
    case 11: return HUD_MAN_DEPART;                       /* start of the route */
    case 10: case 12: case 24: case 25: case 27: return HUD_MAN_ARRIVE;   /* arrived (left / right), end of directions */
    case 15: case 16: case 17: return HUD_MAN_STRAIGHT;   /* ferry: no picture of ours */
    case 6: case 7: case 19: return HUD_MAN_ROUNDABOUT;   /* enter, exit, U-turn at a roundabout */
    default:
        if (type >= 28 && type <= 46) return HUD_MAN_ROUNDABOUT;
        return by_angle(angle);                           /* 0 no turn, 3 straight, 5 follow road, future types */
    }
}

/* st1ng's laneKind(): straight up to 20 degrees, slight up to 59, turn up to 159, then U-turn. */
unsigned hud_rg_lane_arrows(const int *ang, int n)
{
    unsigned bits = 0;
    int i;
    for (i = 0; i < n; i++) {
        int a = ang[i], m = a < 0 ? -a : a;
        if (m <= 20) bits |= HUD_LANE_STRAIGHT;
        else if (m < 60) bits |= a < 0 ? HUD_LANE_SLIGHT_LEFT : HUD_LANE_SLIGHT_RIGHT;
        else if (m < 160) bits |= a < 0 ? HUD_LANE_LEFT : HUD_LANE_RIGHT;
        else bits |= HUD_LANE_UTURN;
    }
    return bits ? bits : HUD_LANE_STRAIGHT;
}

enum { RG_MAX_LANES = 8 };

/* "status/highlight/angle,angle;..." -> lanes; at most HUD_MAX_LANES, a window that keeps the lanes to take. */
static void parse_lanes(hud_nav_info *nav, const char *v, const char *e)
{
    hud_lane all[RG_MAX_LANES];
    int n = 0, first = -1, last = -1, start = 0, i, ok;
    while (v < e && n < RG_MAX_LANES) {
        int status = num(&v, e, &ok), hl = 1000, ang[4], na = 0;
        hud_lane *L = &all[n];
        if (!ok) break;
        if (v < e && *v == '/') { v++; hl = num(&v, e, &ok); if (!ok) hl = 1000; }
        if (v < e && *v == '/') {
            v++;
            while (v < e && *v != ';') {
                int a = num(&v, e, &ok);
                if (ok && na < 4) ang[na++] = a;
                if (v < e && *v == ',') v++;
                else break;
            }
        }
        while (v < e && *v != ';') v++;
        if (v < e) v++;
        memset(L, 0, sizeof *L);
        L->kind = HUD_LANE_CAR;
        L->arrows = (uint8_t)hud_rg_lane_arrows(ang, na);
        L->recommended = status != 0;
        if (L->recommended && hl != 1000) {
            unsigned u = hud_rg_lane_arrows(&hl, 1) & L->arrows;
            L->use = (uint8_t)(u == L->arrows ? 0 : u);
        }
        if (L->recommended) { if (first < 0) first = n; last = n; }
        n++;
    }
    if (n > HUD_MAX_LANES && first >= 0) {
        int mid = (first + last) / 2;
        start = mid - HUD_MAX_LANES / 2 + 1;
        if (start > first) start = first;
        if (start < 0) start = 0;
        if (start > n - HUD_MAX_LANES) start = n - HUD_MAX_LANES;
    }
    nav->nlanes = n - start > HUD_MAX_LANES ? HUD_MAX_LANES : n - start;
    for (i = 0; i < nav->nlanes; i++) nav->lane[i] = all[start + i];
}

/* The phone's text, with its unit when the text is a bare number ("450" + 2 -> "450 m"). */
static void dist_text(hud_rg *r, const char *s)
{
    static const char *const k_units[5] = { "km", "mi", "m", "yd", "ft" };
    int letter = 0;
    const char *c;
    for (c = s; *c; c++)
        if ((*c | 32) >= 'a' && (*c | 32) <= 'z') letter = 1;
        else if ((unsigned char)*c >= 0x80) letter = 1;   /* "м", "км" */
    if (!*s) r->dist_text[0] = 0;
    else if (letter || r->units < 0 || r->units > 4) snprintf(r->dist_text, sizeof r->dist_text, "%s", s);
    else snprintf(r->dist_text, sizeof r->dist_text, "%s %s", s, k_units[r->units]);
}

int hud_rg_parse(hud_rg *r, const char *text, size_t len)
{
    const char *l = text, *end = text + len, *v;
    char cur[HUD_STREET_MAX] = "", road[HUD_STREET_MAX] = "", ds[HUD_RG_DIST_TEXT] = "";
    int seen = 0;
    memset(r, 0, sizeof *r);
    r->route_state = -1;
    r->units = -1;
    while (l < end && *l) {
        const char *e = l;
        while (e < end && *e && *e != '\n') e++;
        if (e > l && e[-1] == '\r') e--;
        if (e - l >= 14 && !memcmp(l, "@routeguidance", 14)) seen = 1;
        else if (key_is(l, e, "route_state", &v)) { r->route_state = num_of(v, e); seen = 1; }
        else if (key_is(l, e, "dist_to_maneuver", &v)) r->nav.dist_m = num_of(v, e);
        else if (key_is(l, e, "dist_to_maneuver_units", &v)) r->units = num_of(v, e);
        else if (key_is(l, e, "dist_to_maneuver_string", &v)) copy_val(ds, sizeof ds, v, e);
        else if (key_is(l, e, "current_road", &v)) copy_val(cur, sizeof cur, v, e);
        else if (key_is(l, e, "maneuver_index", &v)) { r->index = (unsigned)num_of(v, e); r->has_maneuver = 1; }
        else if (key_is(l, e, "maneuver_type", &v)) r->type = (unsigned)num_of(v, e);
        else if (key_is(l, e, "maneuver_exit_angle", &v)) r->angle = num_of(v, e);
        else if (key_is(l, e, "maneuver_driving_side", &v)) r->side = num_of(v, e);
        else if (key_is(l, e, "maneuver_road", &v)) copy_val(road, sizeof road, v, e);
        else if (key_is(l, e, "lanes", &v)) parse_lanes(&r->nav, v, e);
        while (e < end && *e && *e != '\n') e++;
        l = e < end && *e ? e + 1 : e;
    }
    if (!seen) return -1;
    r->nav.man = hud_rg_maneuver(r->type, r->angle, r->side, &r->nav.exit);
    r->nav.exit_angle = r->nav.man == HUD_MAN_ROUNDABOUT ? hud_rg_roundabout_angle(r->angle) : 0;
    if (r->nav.man == HUD_MAN_ROUNDABOUT && r->type == 19 && !r->nav.exit_angle) r->nav.exit_angle = 345;
    if (r->nav.dist_m < 0) r->nav.dist_m = 0;
    r->nav.total_m = r->nav.dist_m;
    snprintf(r->nav.street, sizeof r->nav.street, "%s", road[0] ? road : cur);
    dist_text(r, ds);
    r->active = r->has_maneuver && (r->route_state == 1 || r->route_state == 5 || r->route_state == 6);
    r->hold = r->route_state == 3 || r->route_state == 4;
    return 0;
}

void hud_rg_track_init(hud_rg_track *t)
{
    memset(t, 0, sizeof *t);
}

int hud_rg_track_update(hud_rg_track *t, const hud_rg *r)
{
    hud_rg_track was = *t;
    if (r && r->active) {
        if (!t->have || r->index != t->index || r->nav.dist_m > t->total_m) t->total_m = r->nav.dist_m;
        t->index = r->index;
        t->have = 1;
        t->nav = r->nav;
        t->nav.total_m = t->total_m;
        t->route_active = 1;
    } else if (r && r->hold && t->have) {
        /* keep the last picture */
    } else {
        t->have = t->route_active = 0;
        t->total_m = 0;
    }
    return was.route_active != t->route_active || (t->route_active && memcmp(&was.nav, &t->nav, sizeof t->nav));
}

void hud_rg_file_init(hud_rg_file *f, const char *path)
{
    memset(f, 0, sizeof *f);
    f->path = path;
    f->ino = -1;
}

int hud_rg_file_poll(hud_rg_file *f)
{
    struct stat sb;
    char buf[2048];
    size_t n;
    FILE *fp;
    if (!f->path || stat(f->path, &sb) != 0) {
        int changed = f->present;
        f->present = 0;
        f->ino = -1;
        return changed;
    }
    if (f->present && f->ino == (long long)sb.st_ino && f->size == (long long)sb.st_size && f->mtime == sb.st_mtime)
        return 0;
    if (!(fp = fopen(f->path, "r"))) {   /* replaced between stat and open: try again next poll */
        int changed = f->present;
        f->present = 0;
        f->ino = -1;
        return changed;
    }
    n = fread(buf, 1, sizeof buf, fp);
    fclose(fp);
    f->ino = (long long)sb.st_ino;
    f->size = (long long)sb.st_size;
    f->mtime = sb.st_mtime;
    f->present = hud_rg_parse(&f->rg, buf, n) == 0;
    return 1;
}
