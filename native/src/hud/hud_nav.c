#include "hud_nav.h"
#include "hud_glyphs.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void hud_text_handles_default(hud_text_handles *t)
{
    t->street = 2;
    t->distance = 3;
}

void hud_nav_distance_text(int dist_m, enum hud_maneuver man, const hud_text_style *ts, char *out, size_t cap)
{
    hud_text_style d;
    if (!ts) { hud_text_style_default(&d); ts = &d; }
    hud_text_distance(dist_m, man == HUD_MAN_ARRIVE, ts, out, cap);
}

/* ---- glyphs, in design units (1 unit = 1 px of the 480x180 layout), y down ---- */

typedef struct { double x, y; } pt2;

enum { PATH_PTS = 48, GLYPH_SHAPES = 8 };

/* Layers draw in order; within a layer every shape has the same colour. */
enum { L_DIM, L_INK, L_TOP };

typedef struct { pt2 p[HUD_MAX_POINTS]; int n, layer; } shape;
typedef struct { shape s[GLYPH_SHAPES]; int n; double x0, y0, x1, y1; } glyph;

/* A pen that walks straight lines and circular arcs. Both edges are exact offsets of the centre line (parallel
   lines, concentric arcs), so an outline never crosses itself while every arc radius exceeds half the width. */
typedef struct { pt2 l[PATH_PTS], r[PATH_PTS]; int n; double x, y, a, w; } path;

static void path_put(path *p)
{
    double rad = p->a * M_PI / 180, nx = sin(rad) * p->w / 2, ny = -cos(rad) * p->w / 2;
    if (p->n >= PATH_PTS) return;
    p->l[p->n].x = p->x + nx; p->l[p->n].y = p->y + ny;
    p->r[p->n].x = p->x - nx; p->r[p->n].y = p->y - ny;
    p->n++;
}

/* Heading a in degrees: 0 east, -90 north. */
static void path_begin(path *p, double x, double y, double a, double w)
{
    p->n = 0;
    p->x = x; p->y = y; p->a = a; p->w = w;
    path_put(p);
}

static void path_line(path *p, double len)
{
    double rad = p->a * M_PI / 180;
    if (len <= 0) return;
    p->x += cos(rad) * len;
    p->y += sin(rad) * len;
    path_put(p);
}

/* turn > 0 turns left (anticlockwise on screen), in 15 degree steps. */
static void path_arc(path *p, double r, double turn)
{
    int i, steps = (int)ceil(fabs(turn) / 15 - 1e-9);
    double side = turn > 0 ? 1 : -1, a0 = p->a, rad = a0 * M_PI / 180;
    double cx = p->x + side * sin(rad) * r, cy = p->y - side * cos(rad) * r;
    for (i = 1; i <= steps; i++) {
        p->a = a0 - turn * i / steps;
        rad = p->a * M_PI / 180;
        p->x = cx - side * sin(rad) * r;
        p->y = cy + side * cos(rad) * r;
        path_put(p);
    }
}

static shape *glyph_new(glyph *g, int layer)
{
    shape *s;
    if (g->n >= GLYPH_SHAPES) return NULL;
    s = &g->s[g->n++];
    s->n = 0;
    s->layer = layer;
    return s;
}

/* The stroke as one polygon: left edge, the head (barb, tip, barb), right edge back. hl 0: no head. sweep pulls the
   barbs back along the stroke, for the swept head of the Mapbox lane icons. */
static void glyph_stroke(glyph *g, const path *p, double hl, double hw, double sweep, int layer)
{
    shape *s = glyph_new(g, layer);
    double rad = p->a * M_PI / 180, dx = cos(rad), dy = sin(rad), nx = sin(rad), ny = -cos(rad);
    int i;
    if (!s || p->n < 2) { if (s) g->n--; return; }
    for (i = 0; i < p->n; i++) s->p[s->n++] = p->l[i];
    if (hl > 0) {
        s->p[s->n].x = p->x + nx * hw / 2 - dx * sweep; s->p[s->n++].y = p->y + ny * hw / 2 - dy * sweep;
        s->p[s->n].x = p->x + dx * hl;                  s->p[s->n++].y = p->y + dy * hl;
        s->p[s->n].x = p->x - nx * hw / 2 - dx * sweep; s->p[s->n++].y = p->y - ny * hw / 2 - dy * sweep;
    }
    for (i = p->n - 1; i >= 0; i--) s->p[s->n++] = p->r[i];
}

static void glyph_poly(glyph *g, const pt2 *pts, int n, double ox, double oy, int layer)
{
    shape *s = glyph_new(g, layer);
    int i;
    if (!s) return;
    for (i = 0; i < n; i++) { s->p[i].x = pts[i].x + ox; s->p[i].y = pts[i].y + oy; }
    s->n = n;
}

static void glyph_bbox(glyph *g)
{
    int i, j;
    g->x0 = g->y0 = 1e9;
    g->x1 = g->y1 = -1e9;
    for (i = 0; i < g->n; i++)
        for (j = 0; j < g->s[i].n; j++) {
            const pt2 *q = &g->s[i].p[j];
            if (q->x < g->x0) g->x0 = q->x;
            if (q->x > g->x1) g->x1 = q->x;
            if (q->y < g->y0) g->y0 = q->y;
            if (q->y > g->y1) g->y1 = q->y;
        }
    if (!g->n) g->x0 = g->y0 = g->x1 = g->y1 = 0;
}

/* The manoeuvre: Mapbox's turn icons (hud_glyphs.h), mirrored for the left-hand versions. Right-hand traffic: the
   U-turn goes left, the roundabout runs anticlockwise. 1 Mapbox unit = 2.5 design units (32 units = 80 px). */
static int roundabout_angle(const hud_nav_info *n)
{
    int a = n->exit_angle > 0 ? n->exit_angle : n->exit <= 1 ? 90 : n->exit == 2 ? 180 : n->exit == 3 ? 270 : 330;
    int i = (a - HUD_ROUNDABOUT_FIRST + HUD_ROUNDABOUT_STEP / 2) / HUD_ROUNDABOUT_STEP;
    return i < 0 ? 0 : i >= HUD_ROUNDABOUT_COUNT ? HUD_ROUNDABOUT_COUNT - 1 : i;
}

static void maneuver_glyph(glyph *g, const hud_nav_info *n)
{
    static const struct { int glyph, mirror; } k_map[HUD_MAN_COUNT] = {
        [HUD_MAN_STRAIGHT] = { HUD_GLYPH_ARROWSTRAIGHT, 0 },
        [HUD_MAN_LEFT] = { HUD_GLYPH_ARROWRIGHT, 1 },          [HUD_MAN_RIGHT] = { HUD_GLYPH_ARROWRIGHT, 0 },
        [HUD_MAN_SLIGHT_LEFT] = { HUD_GLYPH_ARROWSLIGHTRIGHT, 1 }, [HUD_MAN_SLIGHT_RIGHT] = { HUD_GLYPH_ARROWSLIGHTRIGHT, 0 },
        [HUD_MAN_UTURN] = { HUD_GLYPH_ARROW180RIGHT, 1 },
        [HUD_MAN_ROUNDABOUT] = { -1, 0 },
        [HUD_MAN_ARRIVE] = { HUD_GLYPH_DESTINATION, 0 },
        [HUD_MAN_SHARP_LEFT] = { HUD_GLYPH_ARROWSHARPRIGHT, 1 }, [HUD_MAN_SHARP_RIGHT] = { HUD_GLYPH_ARROWSHARPRIGHT, 0 },
        [HUD_MAN_KEEP_LEFT] = { HUD_GLYPH_FORK, 1 },           [HUD_MAN_KEEP_RIGHT] = { HUD_GLYPH_FORK, 0 },
        [HUD_MAN_EXIT_LEFT] = { HUD_GLYPH_OFFRAMP, 1 },        [HUD_MAN_EXIT_RIGHT] = { HUD_GLYPH_OFFRAMP, 0 },
        [HUD_MAN_MERGE] = { HUD_GLYPH_MERGE, 0 },
        [HUD_MAN_DEPART] = { HUD_GLYPH_STARTING, 0 },
    };
    const double k = 2.5 / HUD_GLYPH_UNIT;
    int m = (unsigned)n->man < HUD_MAN_COUNT ? (int)n->man : HUD_MAN_STRAIGHT, i, j;
    const hud_glyph *src = k_map[m].glyph < 0 ? &hud_roundabouts[roundabout_angle(n)] : &hud_glyphs[k_map[m].glyph];
    double sx = k_map[m].mirror ? -k : k;
    g->n = 0;
    for (i = 0; i < src->n && i < GLYPH_SHAPES; i++) {
        const hud_glyph_poly *q = &src->poly[i];
        shape *sh = glyph_new(g, q->tone == 0 ? L_INK : q->tone == 1 ? L_DIM : L_TOP);
        for (j = 0; j < q->n && j < HUD_MAX_POINTS; j++) {
            sh->p[j].x = q->xy[2 * j] * sx;
            sh->p[j].y = q->xy[2 * j + 1] * k;
        }
        sh->n = j;
    }
    glyph_bbox(g);
}

/* A lane: every arrow it allows grows from one shared stem, like the painted arrows, with Mapbox's lane look: thin
   stems, wide swept heads, the way the route takes white and the lane's other ways grey. */
static void lane_glyph(glyph *g, const hud_lane *ln)
{
    const double w = 4.6, hw = 11.5, hl = 9, sw = 1.8;
    unsigned arrows = ln->arrows, use = ln->use ? ln->use & ln->arrows : ln->arrows;
    int uturn_too = (arrows & HUD_LANE_UTURN) && (arrows & ~HUD_LANE_UTURN);
#define TONE(bit) (ln->recommended && (use & (bit)) ? L_INK : L_DIM)
    path p;
    g->n = 0;
    if (arrows & HUD_LANE_STRAIGHT) {
        path_begin(&p, 0, 0, -90, w); path_line(&p, 19);
        glyph_stroke(g, &p, hl, hw, sw, TONE(HUD_LANE_STRAIGHT));
    }
    if (arrows & HUD_LANE_LEFT) {   /* higher when a U-turn hooks below it */
        path_begin(&p, 0, 0, -90, w); path_line(&p, uturn_too ? 21 : 12); path_arc(&p, 4.5, 90); path_line(&p, 2.5);
        glyph_stroke(g, &p, hl, hw, sw, TONE(HUD_LANE_LEFT));
    }
    if (arrows & HUD_LANE_RIGHT) {
        path_begin(&p, 0, 0, -90, w); path_line(&p, 12); path_arc(&p, 4.5, -90); path_line(&p, 2.5);
        glyph_stroke(g, &p, hl, hw, sw, TONE(HUD_LANE_RIGHT));
    }
    if (arrows & HUD_LANE_SLIGHT_LEFT) {
        path_begin(&p, 0, 0, -90, w); path_line(&p, 9); path_arc(&p, 10, 45); path_line(&p, 4);
        glyph_stroke(g, &p, hl, hw, sw, TONE(HUD_LANE_SLIGHT_LEFT));
    }
    if (arrows & HUD_LANE_SLIGHT_RIGHT) {
        path_begin(&p, 0, 0, -90, w); path_line(&p, 9); path_arc(&p, 10, -45); path_line(&p, 4);
        glyph_stroke(g, &p, hl, hw, sw, TONE(HUD_LANE_SLIGHT_RIGHT));
    }
    if (arrows & HUD_LANE_UTURN) {
        if (uturn_too) {   /* a small hook off the lower stem, ending above the baseline, clear of the other heads */
            path_begin(&p, 0, 0, -90, w); path_line(&p, 8); path_arc(&p, 5.5, 180);
            glyph_stroke(g, &p, 7, 9.5, 1.2, TONE(HUD_LANE_UTURN));
        } else {
            path_begin(&p, 0, 0, -90, w); path_line(&p, 12); path_arc(&p, 6, 180); path_line(&p, 2);
            glyph_stroke(g, &p, hl, hw, sw, TONE(HUD_LANE_UTURN));
        }
    }
#undef TONE
    glyph_bbox(g);
}

/* Lane markers, 10 units tall, bottom centre at 0,0: the bus lane letter А, the tram lane rails. */
static void marker_glyph(glyph *g, int kind, int layer)
{
    static const pt2 k_a[7] = { { -5, 0 }, { -1.1, -10 }, { 1.1, -10 }, { 5, 0 }, { 2.9, 0 }, { 0, -7.3 }, { -2.9, 0 } };
    static const pt2 k_a_bar[4] = { { -2.6, -3.6 }, { 2.6, -3.6 }, { 2.6, -2.1 }, { -2.6, -2.1 } };
    static const pt2 k_rail_ties[28] = {   /* the left rail with three sleepers through both rails, one outline */
        { -4.6, 0 }, { -4.6, -1.4 }, { -6.5, -1.4 }, { -6.5, -2.6 }, { -4.6, -2.6 }, { -4.6, -4.9 }, { -6.5, -4.9 },
        { -6.5, -6.1 }, { -4.6, -6.1 }, { -4.6, -8.4 }, { -6.5, -8.4 }, { -6.5, -9.6 }, { -4.6, -9.6 }, { -4.6, -11 },
        { -3.2, -11 }, { -3.2, -9.6 }, { 6.5, -9.6 }, { 6.5, -8.4 }, { -3.2, -8.4 }, { -3.2, -6.1 }, { 6.5, -6.1 },
        { 6.5, -4.9 }, { -3.2, -4.9 }, { -3.2, -2.6 }, { 6.5, -2.6 }, { 6.5, -1.4 }, { -3.2, -1.4 }, { -3.2, 0 },
    };
    static const pt2 k_rail[4] = { { 3.2, 0 }, { 3.2, -11 }, { 4.6, -11 }, { 4.6, 0 } };
    g->n = 0;
    if (kind == HUD_LANE_BUS) {
        glyph_poly(g, k_a, 7, 0, 0, layer);
        glyph_poly(g, k_a_bar, 4, 0, 0, layer);
    } else if (kind == HUD_LANE_TRAM) {
        glyph_poly(g, k_rail_ties, 28, 0, 0, layer);
        glyph_poly(g, k_rail, 4, 0, 0, layer);
    }
    glyph_bbox(g);
}

/* ---- display list: polygons in HUD pixels, drawn layer by layer ---- */

enum { LIST_MAX = HUD_MAX_CALLS };

typedef struct {
    struct { hud_point p[HUD_MAX_POINTS]; int n, layer; uint8_t colour; } it[LIST_MAX];
    int n, over;
    int w, h;   /* canvas, for clamping */
} dlist;

static int16_t clampi(double v, int hi)
{
    long r = lround(v);
    return (int16_t)(r < 0 ? 0 : r > hi ? hi : r);
}

/* Places glyph g with scale s so that design point (ax, ay) of its bbox lands on (x, y) in HUD pixels;
   ax/ay are fractions of the bbox (0.5, 1 = bottom centre). */
static void dl_glyph(dlist *d, const glyph *g, double s, double x, double y, double ax, double ay,
                     uint8_t dim, uint8_t ink)
{
    double ox = x - (g->x0 + (g->x1 - g->x0) * ax) * s, oy = y - (g->y0 + (g->y1 - g->y0) * ay) * s;
    int i, j;
    for (i = 0; i < g->n; i++) {
        const shape *sh = &g->s[i];
        int k = 0;
        if (d->n >= LIST_MAX) { d->over = 1; return; }
        for (j = 0; j < sh->n; j++) {
            hud_point q;
            q.x = clampi(ox + sh->p[j].x * s, d->w - 1);
            q.y = clampi(oy + sh->p[j].y * s, d->h - 1);
            if (k && q.x == d->it[d->n].p[k - 1].x && q.y == d->it[d->n].p[k - 1].y) continue;
            d->it[d->n].p[k++] = q;
        }
        if (k > 1 && d->it[d->n].p[k - 1].x == d->it[d->n].p[0].x && d->it[d->n].p[k - 1].y == d->it[d->n].p[0].y) k--;
        if (k < 3) continue;   /* too small to show at this scale */
        d->it[d->n].n = k;
        d->it[d->n].layer = sh->layer;
        d->it[d->n].colour = sh->layer == L_INK ? ink : dim;
        d->n++;
    }
}

static void dl_rect(dlist *d, double x0, double y0, double x1, double y1, int layer, uint8_t colour)
{
    int16_t ax = clampi(x0, d->w - 1), bx = clampi(x1, d->w - 1), ay = clampi(y0, d->h - 1), by = clampi(y1, d->h - 1);
    if (bx <= ax) bx = (int16_t)(ax + 1);
    if (by <= ay) by = (int16_t)(ay + 1);
    if (d->n >= LIST_MAX) { d->over = 1; return; }
    d->it[d->n].p[0].x = ax; d->it[d->n].p[0].y = ay;
    d->it[d->n].p[1].x = bx; d->it[d->n].p[1].y = ay;
    d->it[d->n].p[2].x = bx; d->it[d->n].p[2].y = by;
    d->it[d->n].p[3].x = ax; d->it[d->n].p[3].y = by;
    d->it[d->n].n = 4;
    d->it[d->n].layer = layer;
    d->it[d->n].colour = colour;
    d->n++;
}

/* Calls the list costs: one per polygon plus one per colour change. */
static int dl_calls(const dlist *d)
{
    int layer, i, calls = 0, cur = -1;
    for (layer = L_DIM; layer <= L_TOP; layer++)
        for (i = 0; i < d->n; i++)
            if (d->it[i].layer == layer) {
                if (d->it[i].colour != cur) { cur = d->it[i].colour; calls++; }
                calls++;
            }
    return calls;
}

static int dl_emit(hud_batch *b, const dlist *d, uint16_t h)
{
    int layer, i, cur = -1;
    uint16_t id = 1;
    for (layer = L_DIM; layer <= L_TOP; layer++)
        for (i = 0; i < d->n; i++) {
            if (d->it[i].layer != layer) continue;
            if (d->it[i].colour != cur) {
                cur = d->it[i].colour;
                if (hud_fill_color(b, h, (uint8_t)cur)) return -1;
            }
            if (hud_polygon(b, h, id++, d->it[i].p, (unsigned)d->it[i].n)) return -1;
        }
    return 0;
}

/* ---- the layout, on the 480x180 design canvas ---- */

/* Boxes in design units (480x180). The standard layout puts the manoeuvre left of the texts; the big one (the HUD's
   120x72 image, texts elsewhere) gives the manoeuvre the whole left part and stacks the bar over the lanes on the right. */
typedef struct { double man_x0, man_x1, man_y0, man_y1, bar_x0, bar_x1, bar_y0, bar_y1, lane_x0, lane_x1, lane_y0, lane_y1; } nav_boxes;
static const nav_boxes k_std = { 244, 320, 14, 102,   330, 466, 86, 90,    250, 470, 112, 168 };
static const nav_boxes k_big = { 244, 332, 5, 175,    340, 474, 15, 23,    340, 474, 45, 175 };
/* big with the HUD's texts on: the distance text covers the top ~quarter (run 4 T3 video) and says the distance, so no bar */
static const nav_boxes k_big_text = { 244, 340, 58, 175,   0, 0, 0, 0,   348, 474, 58, 175 };
#define NAV_BOXES(g) ((g)->nav_x0 ? (g)->nav_text_top ? &k_big_text : &k_big : &k_std)
#define MAN_X0 (L->man_x0)
#define MAN_X1 (L->man_x1)
#define MAN_Y0 (L->man_y0)
#define MAN_Y1 (L->man_y1)
#define BAR_X0 (L->bar_x0)
#define BAR_X1 (L->bar_x1)
#define BAR_Y0 (L->bar_y0)
#define BAR_Y1 (L->bar_y1)
#define LANE_X0 (L->lane_x0)
#define LANE_X1 (L->lane_x1)
#define LANE_Y0 (L->lane_y0)
#define LANE_Y1 (L->lane_y1)
#define LANE_W_MAX 36.0
#define MARK_H 10.0       /* marker band at the bottom of the lane row, when a lane has a marker */
#define MARK_GAP 4.0

/* Detail is dropped in this order when a frame would not fit the batch. */
enum { D_FULL, D_NO_SEPARATORS, D_NO_MARKERS, D_NO_LANES, D_LEVELS };

static int bar_px(const hud_nav_info *n, int bw)
{
    double f = n->total_m > 0 ? 1.0 - (double)n->dist_m / n->total_m : 1.0;
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    return (int)lround(bw * f);
}

/* Canvas px per design unit across; the design's x nav_x0 lands on canvas x 0. */
static double nav_sx(const hud_geom *g) { return (double)g->canvas_w / (480 - g->nav_x0); }

static void build(dlist *d, const hud_nav_info *n, const hud_palette *pal, const hud_geom *g, int level)
{
    double sx = nav_sx(g), sy = (double)g->canvas_h / 180, s = sx < sy ? sx : sy, ox = g->nav_x0;
    const nav_boxes *L = NAV_BOXES(g);
    int i, nl = n->nlanes < 0 ? 0 : n->nlanes > HUD_MAX_LANES ? HUD_MAX_LANES : n->nlanes, marks = 0;
    int bw = (int)lround((BAR_X1 - BAR_X0) * sx), filled = bar_px(n, bw);
    glyph gl;
    d->n = d->over = 0;
    d->w = g->canvas_w;
    d->h = g->canvas_h;

    /* the manoeuvre, centred in its box, never larger than designed */
    maneuver_glyph(&gl, n);
    {
        /* 1 design unit = 1 px; the big layout lets it grow to its box (run 3: the arrow stayed small on the HUD) */
        double bw_ = (MAN_X1 - MAN_X0) * sx, bh = (MAN_Y1 - MAN_Y0) * sy, gs = g->nav_x0 ? 1e9 : s;
        if ((gl.x1 - gl.x0) * gs > bw_) gs = bw_ / (gl.x1 - gl.x0);
        if ((gl.y1 - gl.y0) * gs > bh) gs = bh / (gl.y1 - gl.y0);
        dl_glyph(d, &gl, gs, ((MAN_X0 + MAN_X1) / 2 - ox) * sx, (MAN_Y0 + MAN_Y1) / 2 * sy, 0.5, 0.5, pal->base_off, pal->ink);
    }

    /* the distance bar: dim track, the lit part grows as the manoeuvre comes closer */
    if (BAR_X1 > BAR_X0) dl_rect(d, (BAR_X0 - ox) * sx, BAR_Y0 * sy, (BAR_X1 - ox) * sx, BAR_Y1 * sy, L_DIM, pal->base_off);
    if (filled > 0) dl_rect(d, (BAR_X0 - ox) * sx, BAR_Y0 * sy, (BAR_X0 - ox) * sx + filled, BAR_Y1 * sy, L_INK, pal->ink);

    if (!nl || level >= D_NO_LANES) return;
    for (i = 0; i < nl; i++) marks |= n->lane[i].kind == HUD_LANE_BUS || n->lane[i].kind == HUD_LANE_TRAM;
    if (level >= D_NO_MARKERS) marks = 0;
    {
        double lw = (LANE_X1 - LANE_X0) / nl < LANE_W_MAX ? (LANE_X1 - LANE_X0) / nl : LANE_W_MAX;
        double x0 = (LANE_X0 + LANE_X1) / 2 - nl * lw / 2;
        double gy1 = LANE_Y1 - (marks ? MARK_H + MARK_GAP : 0), gh = gy1 - LANE_Y0;
        if (level < D_NO_SEPARATORS)
            for (i = 1; i < nl; i++)
                dl_rect(d, (x0 + i * lw - ox) * sx, (LANE_Y0 + 2) * sy, (x0 + i * lw - ox) * sx + (sx < 1 ? 1 : sx),
                        LANE_Y1 * sy, L_DIM, pal->base_off);
        for (i = 0; i < nl; i++) {
            const hud_lane *ln = &n->lane[i];
            int layer = ln->recommended ? L_INK : L_DIM;
            double cx = (x0 + (i + 0.5) * lw - ox) * sx, ls = g->nav_x0 ? 2.0 * s : 1.15 * s;   /* big: fill the lane */
            lane_glyph(&gl, ln);
            if (gl.n) {   /* fit the lane, keep the stems on one baseline */
                if ((gl.x1 - gl.x0) * ls > lw * 0.78 * sx) ls = lw * 0.78 * sx / (gl.x1 - gl.x0);
                if ((gl.y1 - gl.y0) * ls > gh * sy) ls = gh * sy / (gl.y1 - gl.y0);
                dl_glyph(d, &gl, ls, cx, gy1 * sy, 0.5, 1, pal->base_off, pal->ink);
            }
            if (marks && (ln->kind == HUD_LANE_BUS || ln->kind == HUD_LANE_TRAM)) {
                marker_glyph(&gl, ln->kind, layer);
                dl_glyph(d, &gl, s, cx, LANE_Y1 * sy, 0.5, 1, pal->base_off, pal->ink);
            }
        }
    }
}

static int draw_image(hud_batch *b, const hud_nav_info *n, const hud_palette *pal, const hud_geom *g, uint16_t h,
                      uint16_t shown_h)
{
    static dlist d;   /* about 20 KB: off the stack */
    int level, room = HUD_MAX_CALLS - (int)b->n - 2;   /* RemoveAll and the switch */
    for (level = D_FULL; level < D_LEVELS; level++) {
        build(&d, n, pal, g, level);
        if (!d.over && dl_calls(&d) <= room) break;
    }
    if (level == D_LEVELS) return -1;
    if (hud_remove_all(b, h) || dl_emit(b, &d, h) || hud_switch_visibility(b, shown_h, h)) return -1;
    return 0;
}

static int same_lanes(const hud_nav_info *n, const hud_nav_sent *s)
{
    int i;
    if (n->nlanes != s->nlanes) return 0;
    for (i = 0; i < n->nlanes && i < HUD_MAX_LANES; i++)
        if (n->lane[i].arrows != s->lane[i].arrows || n->lane[i].kind != s->lane[i].kind ||
            n->lane[i].recommended != s->lane[i].recommended || n->lane[i].use != s->lane[i].use) return 0;
    return 1;
}

int hud_nav_frame(hud_batch *b, const hud_nav_info *n, const hud_palette *pal, const hud_geom *g,
                  const hud_handles *h, const hud_text_handles *th, const hud_text_style *ts,
                  hud_nav_sent *sent, int *shown)
{
    unsigned start = b->n;
    char street[HUD_TEXT_BYTES], dist[HUD_TEXT_BYTES];
    const nav_boxes *L = NAV_BOXES(g);
    int px = bar_px(n, (int)lround((BAR_X1 - BAR_X0) * nav_sx(g))), image;
    hud_text_style def;
    if (!ts) { hud_text_style_default(&def); ts = &def; }
    hud_text_street(n->street, ts, street, sizeof street);
    hud_nav_distance_text(n->dist_m, n->man, ts, dist, sizeof dist);
    if (!sent->have_text || strcmp(sent->street, street))
        if (hud_text_utf8(b, th->street, street)) goto full;
    if (!sent->have_text || strcmp(sent->distance, dist))
        if (hud_text_utf8(b, th->distance, dist)) goto full;
    image = !sent->have_image || sent->man != (int)n->man || sent->exit != n->exit ||
            sent->exit_angle != n->exit_angle || sent->bar_px != px ||
            !same_lanes(n, sent);
    if (image) {
        uint16_t hidden = h->buf[1 - *shown];
        if (draw_image(b, n, pal, g, hidden, h->buf[*shown])) goto full;
        *shown = 1 - *shown;
        sent->man = (int)n->man;
        sent->exit = n->exit;
        sent->exit_angle = n->exit_angle;
        sent->bar_px = px;
        sent->nlanes = n->nlanes;
        memcpy(sent->lane, n->lane, sizeof sent->lane);
        sent->have_image = 1;
    }
    snprintf(sent->street, sizeof sent->street, "%s", street);
    snprintf(sent->distance, sizeof sent->distance, "%s", dist);
    sent->have_text = 1;
    return (int)(b->n - start);
full:
    b->n = start;
    return -1;
}

int hud_nav_clear(hud_batch *b, const hud_text_handles *th, hud_nav_sent *sent)
{
    unsigned start = b->n;
    if (hud_text_utf8(b, th->street, "") || hud_text_utf8(b, th->distance, "")) {
        b->n = start;
        return -1;
    }
    memset(sent, 0, sizeof *sent);
    return (int)(b->n - start);
}
