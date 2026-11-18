#include "hud_strip.h"

#include <math.h>
#include <string.h>

void hud_profile_b58(hud_profile *p)
{
    p->lo = 1000;
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
}

void hud_geom_default(hud_geom *g)
{
    g->canvas_w = 480;
    g->canvas_h = 180;
    g->x = 258;
    g->y = 54;
    g->w = 208;
    g->h = 30;
    g->gap = 3;
    g->slant = 7;
    g->gear_cx = 362;
    g->gear_top = 104;
    g->gear_h = 44;
    g->nav_x0 = 0;
    g->nav_text_top = 0;
    g->rise = 0;
    g->rpm_x1 = g->rpm_h = g->solid_digits = 0;
}

void hud_geom_for_canvas(hud_geom *g, int canvas_w, int canvas_h)
{
    hud_geom d;
    double sx, sy;
    hud_geom_default(&d);
    sx = (double)canvas_w / d.canvas_w;
    sy = (double)canvas_h / d.canvas_h;
    g->canvas_w = canvas_w;
    g->canvas_h = canvas_h;
    g->x = (int)lround(d.x * sx);
    g->y = (int)lround(d.y * sy);
    g->w = (int)lround(d.w * sx);
    g->h = (int)lround(d.h * sy);
    g->gap = d.gap * sx < 1 ? 1 : (int)lround(d.gap * sx);
    g->slant = (int)lround(d.slant * sx);
    g->gear_cx = (int)lround(d.gear_cx * sx);
    g->gear_top = (int)lround(d.gear_top * sy);
    g->gear_h = (int)lround(d.gear_h * sy);
    g->nav_x0 = 0;
    g->nav_text_top = 0;
    g->rise = 0;
    g->rpm_x1 = g->rpm_h = g->solid_digits = 0;
}

void hud_geom_big(hud_geom *g, int canvas_w, int canvas_h)
{
    g->canvas_w = canvas_w;
    g->canvas_h = canvas_h;
    g->x = canvas_w / 60 + 1;
    g->w = canvas_w - 2 * g->x;
    g->y = canvas_h * 11 / 72 + 1;   /* room for the peak marker above */
    g->h = canvas_h * 24 / 72;
    g->gap = canvas_w / 120 + 1;
    g->slant = canvas_w / 20;
    g->gear_cx = canvas_w / 2;
    g->gear_top = g->y + g->h + canvas_h / 18;
    g->gear_h = canvas_h - g->gear_top - 2;
    g->nav_x0 = 240;   /* the nav design lives in x 244..470 */
    g->rise = g->h / 4;
    g->rpm_x1 = g->rpm_h = g->solid_digits = 0;
}

void hud_geom_split(hud_geom *g, int canvas_w, int canvas_h)
{
    hud_geom_big(g, canvas_w, canvas_h);
    g->gear_cx = g->x + g->gear_h * 6 / 11 / 2;   /* left edge on the strip's */
    g->rpm_x1 = g->x + g->w;
    g->rpm_h = g->gear_h * 4 / 5;                 /* a step smaller: the gear leads */
    g->solid_digits = 1;
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

/* Field i as a parallelogram, the first t of its width. */
static int field_top(const hud_geom *g, int i) { return g->y - g->rise * hud_field_zone(i) / 2; }

static void field_poly(const hud_geom *g, int i, double t, hud_point pt[4])
{
    double fw = field_width(g), x0 = g->x + i * (fw + g->gap);
    int top = field_top(g, i), bot = g->y + g->h;
    double sl = g->h ? (double)g->slant * (bot - top) / g->h : g->slant;   /* same lean when taller */
    pt[0].x = (int16_t)lround(x0 + sl);          pt[0].y = (int16_t)top;
    pt[1].x = (int16_t)lround(x0 + sl + fw * t); pt[1].y = (int16_t)top;
    pt[2].x = (int16_t)lround(x0 + fw * t);            pt[2].y = (int16_t)bot;
    pt[3].x = (int16_t)lround(x0);                     pt[3].y = (int16_t)bot;
}

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

/* Segments a..g of a 7-segment digit. Bit 0 is a. */
static const uint8_t k_seg[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };

static void seg_poly(const hud_geom *g, int s, hud_point pt[4])
{
    int h = g->gear_h, w = h * 6 / 11, t = h / 7 < 2 ? 2 : h / 7;
    int x0 = g->gear_cx - w / 2, y0 = g->gear_top, ym = y0 + h / 2 - t / 2, y1 = y0 + h - t;
    int x = 0, y = 0, sw = 0, sh = 0;
    switch (s) {
    case 0: x = x0;         y = y0; sw = w; sh = t; break;              /* a */
    case 1: x = x0 + w - t; y = y0; sw = t; sh = h / 2; break;          /* b */
    case 2: x = x0 + w - t; y = ym; sw = t; sh = h - (ym - y0); break;  /* c */
    case 3: x = x0;         y = y1; sw = w; sh = t; break;              /* d */
    case 4: x = x0;         y = ym; sw = t; sh = h - (ym - y0); break;  /* e */
    case 5: x = x0;         y = y0; sw = t; sh = h / 2; break;          /* f */
    default: x = x0;        y = ym; sw = w; sh = t; break;              /* g */
    }
    pt[0].x = (int16_t)x;        pt[0].y = (int16_t)y;
    pt[1].x = (int16_t)(x + sw); pt[1].y = (int16_t)y;
    pt[2].x = (int16_t)(x + sw); pt[2].y = (int16_t)(y + sh);
    pt[3].x = (int16_t)x;        pt[3].y = (int16_t)(y + sh);
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

static int digit_w(int h) { return h * 6 / 11; }
static int digit_t(int h) { return h / 7 < 2 ? 2 : h / 7; }

/* Pen state is per image, so set the colour only when it changes. */
static int pen(hud_batch *b, uint16_t h, int *cur, uint8_t c)
{
    if (*cur == c) return 0;
    *cur = c;
    return hud_fill_color(b, h, c);
}

static int draw_backs(hud_batch *b, uint16_t h, const hud_palette *pal, const hud_geom *g,
                      const hud_strip_key *k, int all, uint16_t *id)
{
    int i, zone, cur = -1, outline = pal->backs == HUD_BACKS_OUTLINE;
    hud_point pt[4];
    if (pal->backs == HUD_BACKS_NONE) return 0;
    if (outline && (hud_line_width(b, h, 1) || hud_line_color(b, h, pal->base_off) || pen(b, h, &cur, 0))) return -1;
    for (zone = 0; zone < 3; zone++)
        for (i = 0; i < HUD_FIELDS; i++) {
            if (hud_field_zone(i) != zone || (!all && i < k->full)) continue;
            field_poly(g, i, 1.0, pt);
            if ((!outline && pen(b, h, &cur, color_of(pal, zone, 0))) || hud_polygon(b, h, (*id)++, pt, 4)) return -1;
        }
    return 0;
}

int hud_strip_frame(hud_batch *b, const hud_profile *p, const hud_palette *pal, const hud_geom *g,
                    const hud_handles *hh, const hud_anim *a, int *shown)
{
    hud_strip_key k = hud_strip_key_of(p, g, a);
    unsigned start = b->n;
    uint16_t h = hh->buf[1 - *shown], id = 1;
    int i, zone, cur = -1, lit = k.flash_on ? k.full : 0;
    hud_point pt[4];
    double fw = field_width(g);

    if (hud_remove_all(b, h)) goto full;
    if (!hh->static_backs) {
        hud_strip_key back = k;
        back.full = lit;
        if (draw_backs(b, h, pal, g, &back, 0, &id)) goto full;
    }
    for (zone = 0; zone < 3; zone++)
        for (i = 0; i < lit; i++) {
            if (hud_field_zone(i) != zone) continue;
            field_poly(g, i, 1.0, pt);
            if (pen(b, h, &cur, k.shifting && pal->shift_on ? pal->shift_on : color_of(pal, zone, 1)) ||
                hud_polygon(b, h, id++, pt, 4)) goto full;
        }
    if (!k.shifting && k.partial_px > 0 && lit < HUD_FIELDS) {
        field_poly(g, lit, k.partial_px / fw, pt);
        if (pen(b, h, &cur, color_of(pal, hud_field_zone(lit), 1)) || hud_polygon(b, h, id++, pt, 4)) goto full;
    }
    if (k.peak_field >= 0) {
        peak_poly(g, k.peak_field, pt);
        if (pen(b, h, &cur, color_of(pal, hud_field_zone(k.peak_field), 1)) || hud_polygon(b, h, id++, pt, 4))
            goto full;
    }
    if (k.gear >= 1 && k.gear <= 9 && g->gear_h > 0) {
        uint8_t ink = !k.shifting ? pal->ink : pal->shift_on ? pal->shift_on : k.flash_on ? pal->red_on : pal->red_off;
        if (g->solid_digits) {
            hud_point dp[HUD_DIGIT_PTS];
            int dw = digit_w(g->gear_h);
            int np = hud_digit_poly(k.gear, g->gear_cx - dw / 2, g->gear_top, dw, g->gear_h, digit_t(g->gear_h), dp);
            if (pen(b, h, &cur, ink) || hud_polygon(b, h, id++, dp, (unsigned)np)) goto full;
        } else
            for (i = 0; i < 7; i++) {
                if (!(k_seg[k.gear] >> i & 1)) continue;
                seg_poly(g, i, pt);
                if (pen(b, h, &cur, ink) || hud_polygon(b, h, id++, pt, 4)) goto full;
            }
    }
    if (k.rpm >= 0) {
        /* Right-aligned, bottom on the gear's, in the ink: the number is read, not the colour. */
        hud_point dp[HUD_DIGIT_PTS];
        int dh = g->rpm_h, dw = digit_w(dh), gap = dh / 8 < 2 ? 2 : dh / 8, top = g->gear_top + g->gear_h - dh;
        int x = g->rpm_x1 - dw, v = k.rpm;
        do {
            int np = hud_digit_poly(v % 10, x, top, dw, dh, digit_t(dh), dp);
            if (pen(b, h, &cur, pal->ink) || hud_polygon(b, h, id++, dp, (unsigned)np)) goto full;
            x -= dw + gap;
            v /= 10;
        } while (v > 0);
    }
    if (hud_switch_visibility(b, hh->buf[*shown], h)) goto full;
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
    uint16_t id = 1;
    hud_strip_key k;
    (void)p;
    memset(&k, 0, sizeof k);
    if (hud_remove_all(b, h->static_h) || draw_backs(b, h->static_h, pal, g, &k, 1, &id)) {
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
