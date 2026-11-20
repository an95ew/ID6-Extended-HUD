#include "hud_strip.h"
#include "hud_cam.h"

#include <math.h>
#include <string.h>

void hud_profile_b58(hud_profile *p)
{
    p->lo = 0;   /* owner 2026-11-19: the bar fills from 0, so idle already lights part of the first field */
    p->redline = 7000;
    p->flash_ms = 125;
    hud_profile_for_shift(p, 7000);
}

void hud_profile_for_shift(hud_profile *p, int shift)
{
    p->shift = shift;
    p->yellow = shift - 2000;
    p->red = shift - 1000;
}

void hud_palette_default(hud_palette *p)
{
    /* Until the swatch test in this car. Seen elsewhere: 4 white (the BMW navigation's fill), 3 grey, 5 orange, and
       1, 9, 10 draw nothing (F54 HUD at night, st1ng/nbtevo-carplay-advanced); the 6WB cluster terminal takes 0..19
       and has no yellow (JayJay3071/nbt-android-auto). So: unlit fields grey, lit white / orange / 2 (orange on the
       F54; red is unknown). */
    p->base_off = 3;
    p->base_on = 4;
    p->yellow_off = 3;
    p->yellow_on = 5;
    p->red_off = 3;
    p->red_on = 2;
    p->ink = 4;
    p->shift_on = 0;
    p->backs = HUD_BACKS_FILL;
    p->alert = 0;   /* no red found on 82/02 yet (run 3); settings red=N once the car shows one */
    p->cam = 0;
}

uint8_t hud_cam_ink(const hud_palette *p) { return p->cam ? p->cam : p->alert ? p->alert : p->ink; }

void hud_geom_split(hud_geom *g, int canvas_w, int canvas_h)
{
    g->canvas_w = canvas_w;
    g->canvas_h = canvas_h;
    g->x = canvas_w / 60 + 1;
    g->w = canvas_w - 2 * g->x;
    g->y = canvas_h * 11 / 72 + 1;   /* room for the peak marker above */
    g->h = canvas_h * 24 / 72;
    g->gap = canvas_w / 120 + 1;
    g->slant = canvas_w / 20;
    g->gear_top = g->y + g->h + canvas_h / 18;
    g->gear_h = canvas_h - g->gear_top - 2;
    g->gear_cx = g->x + g->gear_h * 6 / 11 / 2;   /* left edge on the strip's */
    g->rise = g->h / 4;
    g->rpm_x1 = g->x + g->w;
    g->rpm_h = g->gear_h * 4 / 5;                 /* a step smaller: the gear leads */
    g->merge_pts = HUD_MAX_POINTS;
}

void hud_handles_default(hud_handles *h)
{
    h->buf[0] = 5;
    h->buf[1] = 6;
    h->static_h = 0;
    h->static_backs = 0;
}

int hud_field_zone(int i) { return i < 6 ? 0 : i < 9 ? 1 : 2; }

void hud_field_span(const hud_profile *p, int i, double *lo, double *hi)
{
    double a, z;
    int k, n;
    switch (hud_field_zone(i)) {
    case 0: a = p->lo; z = p->yellow; k = i; n = 6; break;
    case 1: a = p->yellow; z = p->red; k = i - 6; n = 3; break;
    default: a = p->red; z = p->shift; k = i - 9; n = 3; break;
    }
    *lo = a + (z - a) * k / n;
    *hi = a + (z - a) * (k + 1) / n;
}

void hud_anim_reset(hud_anim *a) { memset(a, 0, sizeof *a); }

void hud_anim_step(hud_anim *a, double raw_rpm, uint32_t now_ms)
{
    uint32_t dt;
    if (raw_rpm < 0) raw_rpm = 0;
    if (!a->have) {
        a->rpm = a->peak = raw_rpm;
        a->peak_ms = a->now_ms = now_ms;
        a->have = 1;
        return;
    }
    dt = now_ms - a->now_ms;
    if (dt > 200) dt = 200;
    a->now_ms = now_ms;
    a->rpm += (raw_rpm - a->rpm) * (1.0 - exp(-(double)dt / 45.0));
    if (a->rpm >= a->peak) {
        a->peak = a->rpm;
        a->peak_ms = now_ms;
    } else if (now_ms - a->peak_ms > 400) {
        a->peak -= 6.0 * dt;
        if (a->peak < a->rpm) a->peak = a->rpm;
    }
}

int hud_anim_shifting(const hud_profile *p, const hud_anim *a) { return a->have && a->rpm >= p->shift - 5; }
int hud_anim_flash_on(const hud_anim *a) { return (a->now_ms / 125u) % 2u == 0; }
int hud_anim_flash_on_p(const hud_profile *p, const hud_anim *a)
{
    return p->flash_ms <= 0 || (a->now_ms / (uint32_t)p->flash_ms) % 2u == 0;
}

static double field_width(const hud_geom *g)
{
    return (double)(g->w - g->slant - g->gap * (HUD_FIELDS - 1)) / HUD_FIELDS;
}

hud_strip_key hud_strip_key_of(const hud_profile *p, const hud_geom *g, const hud_anim *a)
{
    hud_strip_key k;
    double fw = field_width(g);
    int i;
    memset(&k, 0, sizeof k);
    k.peak_field = -1;
    k.gear = a->gear;
    k.rpm = g->rpm_h > 0 && a->have ? hud_rpm_shown(a->rpm) : -1;
    k.gear_alert = a->gear_alert != 0;
    k.cam_limit = a->cam_limit > 0 && a->cam_limit < 1000 ? a->cam_limit : 0;
    if (k.cam_limit) k.rpm = -1;   /* the limit takes the readout's place */
    k.shifting = hud_anim_shifting(p, a);
    k.flash_on = k.shifting ? hud_anim_flash_on_p(p, a) : 1;
    if (k.shifting) {
        k.full = HUD_FIELDS;
        return k;
    }
    for (i = 0; i < HUD_FIELDS; i++) {
        double lo, hi;
        hud_field_span(p, i, &lo, &hi);
        if (a->rpm >= hi) k.full = i + 1;
        else {
            if (a->rpm > lo) k.partial_px = (int)(fw * (a->rpm - lo) / (hi - lo));
            break;
        }
    }
    if (a->peak > a->rpm + 40)
        for (i = 0; i < HUD_FIELDS; i++) {
            double lo, hi;
            hud_field_span(p, i, &lo, &hi);
            if (a->peak >= lo && a->peak < hi && i >= k.full) { k.peak_field = i; break; }
        }
    return k;
}

int hud_strip_key_equal(const hud_strip_key *a, const hud_strip_key *b) { return !memcmp(a, b, sizeof *a); }

/* Field i as a parallelogram, from t0 to t1 of its width. */
static int field_top(const hud_geom *g, int i) { return g->y - g->rise * hud_field_zone(i) / 2; }

static void field_part(const hud_geom *g, int i, double t0, double t1, hud_point pt[4])
{
    double fw = field_width(g), x0 = g->x + i * (fw + g->gap);
    int top = field_top(g, i), bot = g->y + g->h;
    double sl = g->h ? (double)g->slant * (bot - top) / g->h : g->slant;   /* same lean when taller */
    pt[0].x = (int16_t)lround(x0 + sl + fw * t0); pt[0].y = (int16_t)top;
    pt[1].x = (int16_t)lround(x0 + sl + fw * t1); pt[1].y = (int16_t)top;
    pt[2].x = (int16_t)lround(x0 + fw * t1);      pt[2].y = (int16_t)bot;
    pt[3].x = (int16_t)lround(x0 + fw * t0);      pt[3].y = (int16_t)bot;
}

/* The first t of field i. */
static void field_poly(const hud_geom *g, int i, double t, hud_point pt[4]) { field_part(g, i, 0, t, pt); }

static void peak_poly(const hud_geom *g, int i, hud_point pt[4])
{
    double fw = field_width(g), x0 = g->x + i * (fw + g->gap) + g->slant;
    int y = field_top(g, i) - 2;
    pt[0].x = (int16_t)lround(x0);      pt[0].y = (int16_t)(y - 3);
    pt[1].x = (int16_t)lround(x0 + fw); pt[1].y = (int16_t)(y - 3);
    pt[2].x = (int16_t)lround(x0 + fw); pt[2].y = (int16_t)y;
    pt[3].x = (int16_t)lround(x0);      pt[3].y = (int16_t)y;
}

static uint8_t color_of(const hud_palette *pal, int zone, int on)
{
    if (zone == 0) return on ? pal->base_on : pal->base_off;
    if (zone == 1) return on ? pal->yellow_on : pal->yellow_off;
    return on ? pal->red_on : pal->red_off;
}

int hud_rpm_shown(double rpm) { return rpm <= 0 ? 0 : (int)(rpm / 50.0 + 0.5) * 50; }

/* Digits on a 3 x 5 grid: columns 0, t, w-t, w and rows 0, t, (h-t)/2, (h+t)/2, h-t, h. The outline runs clockwise
   (y down), each hole the other way, entered and left on a horizontal bridge. -1 ends a digit. */
static const signed char k_digit[10][HUD_DIGIT_PTS * 2 + 1] = {
    { 0,0, 3,0, 3,5, 0,5, 0,4, 1,4, 2,4, 2,1, 1,1, 1,4, 0,4, -1 },
    { 2,0, 3,0, 3,5, 2,5, -1 },
    { 0,0, 3,0, 3,3, 1,3, 1,4, 3,4, 3,5, 0,5, 0,2, 2,2, 2,1, 0,1, -1 },
    { 0,0, 3,0, 3,5, 0,5, 0,4, 2,4, 2,3, 0,3, 0,2, 2,2, 2,1, 0,1, -1 },
    { 0,0, 1,0, 1,2, 2,2, 2,0, 3,0, 3,5, 2,5, 2,3, 0,3, -1 },
    { 0,0, 3,0, 3,1, 1,1, 1,2, 3,2, 3,5, 0,5, 0,4, 2,4, 2,3, 0,3, -1 },
    { 0,0, 3,0, 3,1, 1,1, 1,2, 3,2, 3,5, 0,5, 0,4, 1,4, 2,4, 2,3, 1,3, 1,4, 0,4, -1 },
    { 0,0, 3,0, 3,5, 2,5, 2,1, 0,1, -1 },
    { 0,0, 3,0, 3,5, 0,5, 0,4, 1,4, 2,4, 2,3, 1,3, 1,4, 0,4, 0,2, 1,2, 2,2, 2,1, 1,1, 1,2, 0,2, -1 },
    { 0,0, 3,0, 3,5, 0,5, 0,4, 2,4, 2,3, 0,3, 0,2, 1,2, 2,2, 2,1, 1,1, 1,2, 0,2, -1 },
};

int hud_digit_poly(int d, int x, int y, int w, int h, int t, hud_point *pt)
{
    int cx[4], cy[6], n;
    if (d < 0 || d > 9) return 0;
    cx[0] = x; cx[1] = x + t; cx[2] = x + w - t; cx[3] = x + w;
    cy[0] = y; cy[1] = y + t; cy[2] = y + (h - t) / 2; cy[3] = cy[2] + t; cy[4] = y + h - t; cy[5] = y + h;
    for (n = 0; k_digit[d][2 * n] >= 0; n++) {
        pt[n].x = (int16_t)cx[(int)k_digit[d][2 * n]];
        pt[n].y = (int16_t)cy[(int)k_digit[d][2 * n + 1]];
    }
    return n;
}

/* Outlines out to one image, one polygon each or merged. Merged: outlines that share a bottom line chain into one
   polygon, each entered and left at its bottom-left corner and joined along that line. A horizontal edge is crossed by
   no scanline (the keyhole digits rely on the same), so the joins draw nothing and the fill is the outlines'. */
typedef struct {
    hud_batch *b;
    uint16_t h, id;
    int cur;                          /* pen colour, -1 unknown; pen state is per image */
    unsigned max;                     /* merge_pts, 0 = no merging */
    hud_point pt[HUD_MAX_POINTS];
    unsigned n;
    int base;
} emitter;

static void emit_init(emitter *e, hud_batch *b, uint16_t h, int merge_pts)
{
    e->b = b;
    e->h = h;
    e->id = 1;
    e->cur = -1;
    e->max = merge_pts > 0 ? (unsigned)(merge_pts > HUD_MAX_POINTS ? HUD_MAX_POINTS : merge_pts) : 0;
    e->n = 0;
}

static int emit_flush(emitter *e)
{
    int r = 0;
    if (e->n) r = hud_polygon(e->b, e->h, e->id++, e->pt, e->n);
    e->n = 0;
    return r;
}

/* The colour only when it changes; whatever waits goes out in the old one first. */
static int emit_pen(emitter *e, uint8_t c)
{
    if (e->cur == c) return 0;
    if (emit_flush(e)) return -1;
    e->cur = c;
    return hud_fill_color(e->b, e->h, c);
}

static int emit_outline(emitter *e, const hud_point *p, unsigned n)
{
    unsigned s = 0, i, need;
    if (!e->max) return hud_polygon(e->b, e->h, e->id++, p, n);
    for (i = 1; i < n; i++)
        if (p[i].y > p[s].y || (p[i].y == p[s].y && p[i].x < p[s].x)) s = i;
    need = n + (p[(s + n - 1) % n].y != p[s].y);   /* back to the bottom line unless it ends there */
    if (e->n && (p[s].y != e->base || e->n + need > e->max) && emit_flush(e)) return -1;
    if (need > e->max) return hud_polygon(e->b, e->h, e->id++, p, n);
    for (i = 0; i < n; i++) e->pt[e->n++] = p[(s + i) % n];
    if (need > n) e->pt[e->n++] = p[s];
    e->base = p[s].y;
    return 0;
}

static int digit_w(int h) { return h * 6 / 11; }
static int digit_t(int h) { return h / 7 < 2 ? 2 : h / 7; }

static int draw_backs(emitter *e, const hud_palette *pal, const hud_geom *g, const hud_strip_key *k, int all)
{
    int i, zone, outline = pal->backs == HUD_BACKS_OUTLINE;
    hud_point pt[4];
    if (pal->backs == HUD_BACKS_NONE) return 0;
    if (outline && (hud_line_width(e->b, e->h, 1) || hud_line_color(e->b, e->h, pal->base_off) || emit_pen(e, 0)))
        return -1;
    for (zone = 0; zone < 3; zone++)
        for (i = 0; i < HUD_FIELDS; i++) {
            if (hud_field_zone(i) != zone || (!all && i < k->full)) continue;
            /* car run 7 (2026-11-19): a lit part painted over its back did not show, so the partly lit field's back
               covers only the unlit rest; no two polygons overlap */
            if (!all && i == k->full && !k->shifting && k->partial_px > 0)
                field_part(g, i, k->partial_px / field_width(g), 1.0, pt);
            else field_poly(g, i, 1.0, pt);
            if ((!outline && emit_pen(e, color_of(pal, zone, 0))) || emit_outline(e, pt, 4)) return -1;
        }
    return 0;
}

int hud_strip_frame(hud_batch *b, const hud_profile *p, const hud_palette *pal, const hud_geom *g,
                    const hud_handles *hh, const hud_anim *a, int *shown)
{
    hud_strip_key k = hud_strip_key_of(p, g, a);
    unsigned start = b->n;
    uint16_t h = hh->buf[1 - *shown];
    int i, zone, lit = k.flash_on ? k.full : 0;
    hud_point pt[4];
    double fw = field_width(g);
    emitter e;

    emit_init(&e, b, h, g->merge_pts);
    if (hud_remove_all(b, h)) goto full;
    if (!hh->static_backs) {
        hud_strip_key back = k;
        back.full = lit;
        if (draw_backs(&e, pal, g, &back, 0)) goto full;
    }
    for (zone = 0; zone < 3; zone++)
        for (i = 0; i < lit; i++) {
            if (hud_field_zone(i) != zone) continue;
            field_poly(g, i, 1.0, pt);
            if (emit_pen(&e, k.shifting && pal->shift_on ? pal->shift_on : color_of(pal, zone, 1)) ||
                emit_outline(&e, pt, 4)) goto full;
        }
    if (!k.shifting && k.partial_px > 0 && lit < HUD_FIELDS) {
        field_poly(g, lit, k.partial_px / fw, pt);
        if (emit_pen(&e, color_of(pal, hud_field_zone(lit), 1)) || emit_outline(&e, pt, 4)) goto full;
    }
    if (k.peak_field >= 0) {
        peak_poly(g, k.peak_field, pt);
        if (emit_pen(&e, color_of(pal, hud_field_zone(k.peak_field), 1)) || emit_outline(&e, pt, 4)) goto full;
    }
    if (k.gear >= 1 && k.gear <= 9 && g->gear_h > 0) {
        uint8_t ink = !k.shifting ? (k.gear_alert && pal->alert ? pal->alert : pal->ink)
                    : pal->shift_on ? pal->shift_on : k.flash_on ? pal->red_on : pal->red_off;
        hud_point dp[HUD_DIGIT_PTS];
        int dw = digit_w(g->gear_h);
        int np = hud_digit_poly(k.gear, g->gear_cx - dw / 2, g->gear_top, dw, g->gear_h, digit_t(g->gear_h), dp);
        if (emit_pen(&e, ink) || emit_outline(&e, dp, (unsigned)np)) goto full;
    }
    if (k.cam_limit && g->rpm_h > 0 && g->gear_h > 0) {
        /* Camera alert with the bar (design approved 2026-11-19): the camera in the gap right of the gear, the limit where
           the revs are, at their size, until 50 m past the camera. Every shape on its own: the HUD draws joins. */
        hud_point icon[HUD_CAM_ICON_POLYS][HUD_CAM_ICON_PTS], dp[HUD_DIGIT_PTS];
        unsigned np[HUD_CAM_ICON_POLYS];
        int dh = g->rpm_h, dw = digit_w(dh), gap = dh / 8 < 2 ? 2 : dh / 8, bottom = g->gear_top + g->gear_h;
        int n = hud_cam_icon(1, g->gear_cx + digit_w(g->gear_h) / 2 + 5, bottom - 27, icon, np), x = g->rpm_x1 - dw;
        int v = k.cam_limit;
        if (emit_pen(&e, hud_cam_ink(pal))) goto full;
        for (i = 0; i < n; i++)
            if (emit_flush(&e) || emit_outline(&e, icon[i], np[i])) goto full;
        do {
            int c = hud_digit_poly(v % 10, x, bottom - dh, dw, dh, digit_t(dh), dp);
            if (emit_pen(&e, pal->cam ? pal->cam : pal->ink) || emit_flush(&e) || emit_outline(&e, dp, (unsigned)c)) goto full;
            x -= dw + gap;
            v /= 10;
        } while (v > 0);
    }
    if (k.rpm >= 0) {
        /* Right-aligned, bottom on the gear's, in the ink: the number is read, not the colour. A polygon per digit:
           the HUD draws edges, and joins across the gaps (gear to RPM, between digits) showed as a line (car run 6). */
        hud_point dp[HUD_DIGIT_PTS];
        int dh = g->rpm_h, dw = digit_w(dh), gap = dh / 8 < 2 ? 2 : dh / 8, top = g->gear_top + g->gear_h - dh;
        int x = g->rpm_x1 - dw, v = k.rpm;
        do {
            int np = hud_digit_poly(v % 10, x, top, dw, dh, digit_t(dh), dp);
            if (emit_pen(&e, pal->ink) || emit_flush(&e) || emit_outline(&e, dp, (unsigned)np)) goto full;
            x -= dw + gap;
            v /= 10;
        } while (v > 0);
    }
    if (emit_flush(&e) || hud_switch_visibility(b, hh->buf[*shown], h)) goto full;
    *shown = 1 - *shown;
    return (int)(b->n - start);
full:
    b->n = start;
    return -1;
}

int hud_strip_static(hud_batch *b, const hud_profile *p, const hud_palette *pal, const hud_geom *g,
                     const hud_handles *h)
{
    unsigned start = b->n;
    hud_strip_key k;
    emitter e;
    (void)p;
    memset(&k, 0, sizeof k);
    emit_init(&e, b, h->static_h, g->merge_pts);
    if (hud_remove_all(b, h->static_h) || draw_backs(&e, pal, g, &k, 1) || emit_flush(&e)) {
        b->n = start;
        return -1;
    }
    return (int)(b->n - start);
}

int hud_strip_clear(hud_batch *b, const hud_handles *h)
{
    unsigned start = b->n;
    if (hud_remove_all(b, h->buf[0]) || hud_remove_all(b, h->buf[1]) ||
        (h->static_backs && hud_remove_all(b, h->static_h))) {
        b->n = start;
        return -1;
    }
    return (int)(b->n - start);
}

int hud_swatches(hud_batch *b, uint16_t h, const hud_geom *g, uint8_t first)
{
    unsigned start = b->n;
    int i, s = g->w / 8 - 4;
    if (hud_remove_all(b, h)) goto full;
    for (i = 0; i < 16; i++) {
        int x = g->x + (i % 8) * (s + 4), y = g->y + (i / 8) * (s + 4);
        hud_point pt[4] = { { (int16_t)x, (int16_t)y }, { (int16_t)(x + s), (int16_t)y },
                            { (int16_t)(x + s), (int16_t)(y + s) }, { (int16_t)x, (int16_t)(y + s) } };
        if (hud_fill_color(b, h, (uint8_t)(first + i)) || hud_polygon(b, h, (uint16_t)(i + 1), pt, 4)) goto full;
    }
    return (int)(b->n - start);
full:
    b->n = start;
    return -1;
}

int hud_swatch_bars(hud_batch *b, uint16_t h, const hud_geom *g, uint8_t first, int n, int line, uint8_t tick)
{
    unsigned start = b->n;
    int i, gap = 4, w, foot = 4, top = 2, bot;
    if (n < 1 || n > 8) return -1;
    w = (g->canvas_w - gap) / n - gap;
    bot = g->canvas_h - foot - 4;
    if (hud_remove_all(b, h)) goto full;
    if (line && (hud_fill_color(b, h, 0) || hud_line_width(b, h, 3))) goto full;
    for (i = 0; i < n; i++) {
        int x = gap + i * (w + gap);
        hud_point pt[4] = { { (int16_t)x, (int16_t)top }, { (int16_t)(x + w), (int16_t)top },
                            { (int16_t)(x + w), (int16_t)bot }, { (int16_t)x, (int16_t)bot } };
        if ((line ? hud_line_color(b, h, (uint8_t)(first + i)) : hud_fill_color(b, h, (uint8_t)(first + i))) ||
            hud_polygon(b, h, (uint16_t)(i + 1), pt, 4)) goto full;
    }
    if (hud_fill_color(b, h, tick) || (line && hud_line_color(b, h, tick))) goto full;
    for (i = 0; i < n; i++) {
        int x = gap + i * (w + gap);
        hud_point pt[4] = { { (int16_t)x, (int16_t)(bot + 3) }, { (int16_t)(x + w), (int16_t)(bot + 3) },
                            { (int16_t)(x + w), (int16_t)(bot + 3 + foot) }, { (int16_t)x, (int16_t)(bot + 3 + foot) } };
        if (hud_polygon(b, h, (uint16_t)(n + i + 1), pt, 4)) goto full;
    }
    return (int)(b->n - start);
full:
    b->n = start;
    return -1;
}

int hud_grid(hud_batch *b, uint16_t h, const hud_geom *g, uint8_t colour)
{
    unsigned start = b->n;
    int i, j;
    if (hud_remove_all(b, h) || hud_fill_color(b, h, colour)) goto full;
    for (j = 0; j < 4; j++)
        for (i = 0; i < 8; i++) {
            int w = 3 + i, hh = 3 + 2 * j;   /* the size says where it is */
            int x = 6 + i * (g->canvas_w - 12 - w) / 7, y = 6 + j * (g->canvas_h - 12 - hh) / 3;
            hud_point pt[4] = { { (int16_t)x, (int16_t)y }, { (int16_t)(x + w), (int16_t)y },
                                { (int16_t)(x + w), (int16_t)(y + hh) }, { (int16_t)x, (int16_t)(y + hh) } };
            if (hud_polygon(b, h, (uint16_t)(j * 8 + i + 1), pt, 4)) goto full;
        }
    return (int)(b->n - start);
full:
    b->n = start;
    return -1;
}

int hud_gear_estimate(double rpm, double kph)
{
    /* ZF 8HP (B58 340i), final drive 3.15, ~2.04 m tyre circumference. Unverified for this car. */
    static const double ratio[8] = { 5.0, 3.2, 2.143, 1.72, 1.314, 1.0, 0.822, 0.64 };
    double r, best = 1e9;
    int i, gear = 0;
    if (rpm < 500 || kph < 5) return 0;
    r = rpm * 2.04 * 60.0 / (kph * 1000.0 * 3.15);
    for (i = 0; i < 8; i++) {
        double e = fabs(r - ratio[i]) / ratio[i];
        if (e < best) { best = e; gear = i + 1; }
    }
    return best <= 0.12 ? gear : 0;
}
