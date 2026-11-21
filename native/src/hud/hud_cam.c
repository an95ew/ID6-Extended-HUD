#include "hud_cam.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265358979323846

int hud_cams_parse(const char *text, size_t len, hud_cam *out, int max)
{
    size_t i = 0;
    int n = 0;
    while (i < len && n < max) {
        char line[160], *p, *e;
        size_t k = 0;
        double lat, lon;
        long limit, dir;
        while (i < len && text[i] != '\n') {
            if (k + 1 < sizeof line) line[k++] = text[i];
            i++;
        }
        i++;
        line[k] = 0;
        for (p = line; *p == ' ' || *p == '\t'; p++) {}
        if (!*p || *p == '#') continue;
        lat = strtod(p, &e);
        if (e == p) continue;
        lon = strtod(p = e, &e);
        if (e == p) continue;
        limit = strtol(p = e, &e, 10);
        if (e == p) continue;
        dir = strtol(p = e, &e, 10);
        if (e == p) dir = -1;
        if (!isfinite(lat) || !isfinite(lon)) continue;   /* strtod takes "nan" and "inf" */
        if (lat < -90 || lat > 90 || lon < -180 || lon > 180 || limit < 0 || limit > 250 || dir < -1 || dir > 359) continue;
        out[n].lat_e6 = (int32_t)floor(lat * 1e6 + 0.5);
        out[n].lon_e6 = (int32_t)floor(lon * 1e6 + 0.5);
        out[n].limit = (int16_t)limit;
        out[n].dir = (int16_t)dir;
        n++;
    }
    return n;
}

void hud_geo_offset(int32_t lat_a, int32_t lon_a, int32_t lat_b, int32_t lon_b, double *east, double *north)
{
    *north = (lat_b - lat_a) * 1e-6 * 111320.0;
    *east = (lon_b - lon_a) * 1e-6 * 111320.0 * cos(lat_a * 1e-6 * PI / 180.0);
}

void hud_cam_track_init(hud_cam_track *t)
{
    memset(t, 0, sizeof *t);
    t->idx = -1;
    t->heading = t->track_heading = -1;
    t->show_m = HUD_CAM_SHOW_M;
}

static int angle_diff(int a, int b)
{
    int d = ((a - b) % 360 + 360) % 360;
    return d > 180 ? 360 - d : d;
}

/* Along the heading (ahead positive) and across it (right positive) from the fix to camera c. */
static void along_across(const hud_geo_fix *f, int heading, const hud_cam *c, double *along, double *across)
{
    double e, n, h = heading * PI / 180.0;
    hud_geo_offset(f->lat_e6, f->lon_e6, c->lat_e6, c->lon_e6, &e, &n);
    *along = e * sin(h) + n * cos(h);
    *across = e * cos(h) - n * sin(h);
}

int hud_cam_track_update(hud_cam_track *t, const hud_cam *cams, int n, const hud_geo_fix *fix)
{
    int was_active = t->active, was_idx = t->idx, was_dist = t->dist_m / 10, i, best = -1;
    int show = t->show_m > 0 ? t->show_m : HUD_CAM_SHOW_M;
    /* the near box, the show distance plus ~170 m (500 m: 6000 x 9000 in 1e-6 degrees, ~670 m) */
    int32_t box_lat = (int32_t)((show + 170) * 1e6 / 111320.0), box_lon = box_lat * 3 / 2;
    double best_along = 0, best_score = 0;
    if (!fix->valid) {
        t->last.valid = t->anchor.valid = 0;
        t->heading = t->track_heading = -1;
        t->active = 0;
        t->idx = -1;
        return was_active;
    }
    if (!t->anchor.valid) t->anchor = *fix;   /* the track's own bearing, kept even with a CDS heading (car run 8 G1) */
    else {
        double e, no;
        hud_geo_offset(t->anchor.lat_e6, t->anchor.lon_e6, fix->lat_e6, fix->lon_e6, &e, &no);
        if (e * e + no * no >= (double)HUD_CAM_HEADING_MOVE_M * HUD_CAM_HEADING_MOVE_M) {
            t->track_heading = ((int)floor(atan2(e, no) * 180.0 / PI + 0.5) + 360) % 360;
            t->anchor = *fix;
        }
    }
    if (fix->heading >= 0) t->heading = fix->heading % 360;
    else if (t->track_heading >= 0) t->heading = t->track_heading;
    t->last = *fix;
    if (t->heading < 0) {
        t->active = 0;
        t->idx = -1;
        return was_active;
    }
    if (t->active) {   /* keep it until 50 m past, or until we leave its road */
        const hud_cam *ac = &cams[t->idx];
        double along, across;
        along_across(fix, t->heading, ac, &along, &across);
        if (along < -HUD_CAM_PAST_M || along > show + 100 || fabs(across) > HUD_CAM_KEEP_SIDE_M ||
            (ac->dir >= 0 && angle_diff(t->heading, ac->dir) > HUD_CAM_DIR_KEEP_DEG))
            t->active = 0;
        else t->dist_m = (int)floor(along + 0.5);
    }
    if (!t->active) {
        for (i = 0; i < n; i++) {
            const hud_cam *c = &cams[i];
            double along, across;
            if (abs(c->lat_e6 - fix->lat_e6) > box_lat || abs(c->lon_e6 - fix->lon_e6) > box_lon) continue;
            if (c->dir >= 0 && angle_diff(t->heading, c->dir) > HUD_CAM_DIR_DEG) continue;   /* the other direction's */
            along_across(fix, t->heading, c, &along, &across);
            if (along <= 0 || along > show + 1) continue;   /* 1 m: the e6 rounding */
            /* ahead: inside the cone, or close to the line of travel when near (a roadside camera 20 m off at 30 m) */
            if (fabs(across) > along * tan(HUD_CAM_AHEAD_DEG * PI / 180.0) && fabs(across) > 25) continue;
            if (fabs(across) > HUD_CAM_SIDE_M) continue;   /* far ahead the cone takes in the parallel roads */
            /* the nearest to our road: a camera 55 m off at 100 m loses to one on the road at 120 m */
            if (best < 0 || along + 2 * fabs(across) < best_score) {
                best = i;
                best_along = along;
                best_score = along + 2 * fabs(across);
            }
        }
        if (best >= 0) {
            t->active = 1;
            t->idx = best;
            t->dist_m = (int)floor(best_along + 0.5);
        } else t->idx = -1;
    }
    t->limit = t->active ? cams[t->idx].limit : 0;
    return t->active != was_active || (t->active && (t->idx != was_idx || t->dist_m / 10 != was_dist));
}

/* ---- the icon: the shapes of the design page (artifact hud-red-camera), on the HUD grid ---- */

typedef struct { hud_point *p; unsigned n; } path;

static void put(path *pa, double x, double y)
{
    hud_point q;
    q.x = (int16_t)floor(x + 0.5);
    q.y = (int16_t)floor(y + 0.5);
    if (pa->n && pa->p[pa->n - 1].x == q.x && pa->p[pa->n - 1].y == q.y) return;
    if (pa->n < HUD_CAM_ICON_PTS) pa->p[pa->n++] = q;
}

/* A full circle from angle a0 back to it, the other way round from the body (a keyhole hole). */
static void loop(path *pa, double cx, double cy, double r, int n, double a0)
{
    int i;
    for (i = 0; i <= n; i++) { double t = a0 - 2 * PI * i / n; put(pa, cx + r * cos(t), cy + r * sin(t)); }
}

static void circle(path *pa, double cx, double cy, double r, int n)
{
    int i;
    for (i = 0; i < n; i++) { double t = 2 * PI * i / n; put(pa, cx + r * cos(t), cy + r * sin(t)); }
}

/* The body with two lens holes: the small one bridged from the right edge, the big one from the left. */
static void body(path *pa, double x, double y, double w, double h, double ch, const double s[3], const double g[3])
{
    put(pa, x, y + g[1]); put(pa, x, y + ch); put(pa, x + ch, y); put(pa, x + w - ch, y); put(pa, x + w, y + ch);
    put(pa, x + w, y + s[1]);
    loop(pa, x + s[0], y + s[1], s[2], 14, 0);
    put(pa, x + w, y + s[1]);
    put(pa, x + w, y + h - ch); put(pa, x + w - ch, y + h); put(pa, x + ch, y + h); put(pa, x, y + h - ch);
    put(pa, x, y + g[1]);
    loop(pa, x + g[0], y + g[1], g[2], 18, PI);
    put(pa, x, y + g[1]);
}

/* One stepped line: up from its foot, which runs right to the mount. */
static void ell(path *pa, double x0, double y0, double x, double top, double foot, double w, double right)
{
    put(pa, x0 + x, y0 + top); put(pa, x0 + x + w, y0 + top); put(pa, x0 + x + w, y0 + foot - w);
    put(pa, x0 + right, y0 + foot - w); put(pa, x0 + right, y0 + foot); put(pa, x0 + x, y0 + foot);
}

static void block(path *pa, double x0, double y0, double x1, double y1)
{
    put(pa, x0, y0); put(pa, x1, y0); put(pa, x1, y1); put(pa, x0, y1);
}

int hud_cam_icon(int badge, int x, int y, hud_point pts[][HUD_CAM_ICON_PTS], unsigned *npts)
{
    path pa[HUD_CAM_ICON_POLYS];
    int i, n;
    for (i = 0; i < HUD_CAM_ICON_POLYS; i++) { pa[i].p = pts[i]; pa[i].n = 0; }
    if (!badge) {
        static const double s[3] = { 16, 7, 3.5 }, g[3] = { 9, 24, 7 };
        body(&pa[0], x, y, 22, 34, 2, s, g);
        circle(&pa[1], x + 16, y + 7, 1.6, 8);
        circle(&pa[2], x + 9, y + 24, 4, 12);
        ell(&pa[3], x, y, 25, 3, 36, 2, 39);
        ell(&pa[4], x, y, 29, 6, 32, 2, 39);
        ell(&pa[5], x, y, 33, 9, 28, 2, 39);
        block(&pa[6], x + 39, y + 13, x + 47, y + 24);
        n = 7;
    } else {
        static const double s[3] = { 10, 4.5, 2 }, g[3] = { 6.5, 16, 4.5 };
        body(&pa[0], x, y, 13, 24, 2, s, g);
        circle(&pa[1], x + 6.5, y + 16, 2.2, 10);
        ell(&pa[2], x, y, 16, 3, 24, 2, 23);
        ell(&pa[3], x, y, 20, 7, 20, 2, 23);
        block(&pa[4], x + 23, y + 8, x + 28, y + 15);
        n = 5;
    }
    for (i = 0; i < n; i++) npts[i] = pa[i].n;
    return n;
}

/* Keyhole ring: the outer circle, a zero-width bridge at angle 0, the inner circle the other way. */
static unsigned ring(hud_point *pt, double cx, double cy, double ro, double ri, int n)
{
    path pa;
    int i;
    pa.p = pt;
    pa.n = 0;
    for (i = 0; i <= n; i++) { double t = 2 * PI * i / n; put(&pa, cx + ro * cos(t), cy + ro * sin(t)); }
    for (i = 0; i <= n; i++) { double t = -2 * PI * i / n; put(&pa, cx + ri * cos(t), cy + ri * sin(t)); }
    return pa.n;
}

static int digit_w(int h) { return h * 6 / 11; }
static int digit_t(int h) { return h / 7 < 2 ? 2 : h / 7; }

int hud_cam_frame(hud_batch *b, const hud_palette *pal, const hud_handles *hh, int limit, int dist_m, int show_m,
                  int *shown)
{
    unsigned start = b->n, np[HUD_CAM_ICON_POLYS];
    uint16_t h = hh->buf[1 - *shown], id = 1;
    hud_point icon[HUD_CAM_ICON_POLYS][HUD_CAM_ICON_PTS], pt[HUD_CAM_ICON_PTS];
    int i, n = hud_cam_icon(0, 5, 14, icon, np);
    if (show_m <= 0) show_m = HUD_CAM_SHOW_M;
    if (hud_remove_all(b, h) || hud_fill_color(b, h, hud_cam_ink(pal))) goto full;
    for (i = 0; i < n; i++)
        if (hud_polygon(b, h, id++, icon[i], np[i])) goto full;
    if (hud_fill_color(b, h, pal->cam ? pal->cam : pal->ink) || hud_polygon(b, h, id++, pt, ring(pt, 88, 32, 24, 19, 28)))
        goto full;
    if (limit > 0 && limit < 1000) {
        int digits = limit >= 100 ? 3 : limit >= 10 ? 2 : 1, dh = digits > 2 ? 15 : 19, dw = digit_w(dh);
        int gap = digits > 2 ? 1 : 3, total = digits * dw + (digits - 1) * gap, x = 88 + total / 2 - dw, v = limit;
        hud_point dp[HUD_DIGIT_PTS];
        do {
            unsigned c = (unsigned)hud_digit_poly(v % 10, x, 32 - dh / 2, dw, dh, digit_t(dh), dp);
            if (hud_polygon(b, h, id++, dp, c)) goto full;
            x -= dw + gap;
            v /= 10;
        } while (v > 0);
    }
    if (dist_m > 0) {   /* shrinks from 112 px at show_m */
        int w = 112 * (dist_m > show_m ? show_m : dist_m) / show_m;
        hud_point bar[4];
        if (w < 2) w = 2;
        if (pal->cam && hud_fill_color(b, h, pal->ink)) goto full;   /* the limit was in the camera's colour */
        bar[0].x = 4; bar[0].y = 62; bar[1].x = (int16_t)(4 + w); bar[1].y = 62;
        bar[2].x = (int16_t)(4 + w); bar[2].y = 67; bar[3].x = 4; bar[3].y = 67;
        if (hud_polygon(b, h, id++, bar, 4)) goto full;
    }
    if (hud_switch_visibility(b, hh->buf[*shown], h)) goto full;
    *shown = 1 - *shown;
    return (int)(b->n - start);
full:
    b->n = start;
    return -1;
}
