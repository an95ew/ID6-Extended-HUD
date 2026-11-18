#include "hud_proto.h"

#include <stdio.h>
#include <string.h>

static unsigned be16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }

static int32_t be32s(const uint8_t *p)
{
    return (int32_t)((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]);
}

static int handle_ok(unsigned h) { return h >= 1 && h <= 255; }

uint16_t hud_call_handle(const hud_call *c) { return c->len >= 2 ? (uint16_t)be16(c->p) : 0; }

int hud_call_valid(const hud_call *c)
{
    const uint8_t *p = c->p;
    unsigned n = c->len;
    if (n < 2 || n > HUD_MAX_PAYLOAD || !handle_ok(be16(p)))
        return 0;
    switch (c->fkt) {
    case HUD_SET_VISIBILITY:
        return n == 3 && p[2] <= 1;
    case HUD_SWITCH_VISIBILITY:
        return n == 4 && handle_ok(be16(p + 2));
    case HUD_TEXT:
        /* handle, 0, UTF-16BE, 0x0000; odd length 5..69 */
        return n >= 5 && n <= 69 && (n & 1) && p[2] == 0 && p[n - 1] == 0 && p[n - 2] == 0;
    case HUD_LINE_COLOR: case HUD_LINE_WIDTH: case HUD_FILL_COLOR: case HUD_FILL_MODE:
        return n == 3;
    case HUD_POLYGON:
        return n > 9 && (p[4] == 2 || p[4] == 3) && p[5] >= 1 && p[5] <= HUD_MAX_POINTS && n == 4u * p[5] + 6;
    case HUD_CIRCLE: case HUD_CIRCLE_DIAMETER: {
        int32_t a, s;
        if (n != 19 || (p[4] != 2 && p[4] != 3))
            return 0;
        a = be32s(p + 11);
        s = be32s(p + 15);
        return a >= -7200 && a <= 7200 && s >= -7200 && s <= 7200;
    }
    case HUD_REMOVE_ALL:
        return n == 2;
    case HUD_GET_SIZE:   /* [h] (6WA, F54) or [h][01] (6WB) */
        return n == 2 || (n == 3 && c->p[2] == 1);
    default:
        return 0;
    }
}

const char *hud_call_name(uint16_t fkt)
{
    switch (fkt) {
    case HUD_SET_VISIBILITY: return "SetVisibility";
    case HUD_SWITCH_VISIBILITY: return "SwitchVisibility";
    case HUD_TEXT: return "TextareaSetText";
    case HUD_LINE_COLOR: return "ImageSetLineColor";
    case HUD_LINE_WIDTH: return "ImageSetLineWidth";
    case HUD_FILL_COLOR: return "ImageSetFillColor";
    case HUD_FILL_MODE: return "ImageSetFillMode";
    case HUD_POLYGON: return "ImageAddPolygon";
    case HUD_CIRCLE: return "ImageAddCircle";
    case HUD_REMOVE_ALL: return "ImageRemoveAllItems";
    case HUD_GET_SIZE: return "ImageGetSize";
    case HUD_CIRCLE_DIAMETER: return "ImageAddCircleDiameter";
    default: return "?";
    }
}

void hud_batch_init(hud_batch *b) { b->n = 0; }

/* Starts a call with the handle written; commit() validates it and keeps it, or drops it. */
static hud_call *begin(hud_batch *b, uint16_t fkt, uint16_t h)
{
    hud_call *c;
    if (b->n >= HUD_MAX_CALLS)
        return NULL;
    c = &b->call[b->n];
    c->fkt = fkt;
    c->p[0] = (uint8_t)(h >> 8);
    c->p[1] = (uint8_t)h;
    c->len = 2;
    return c;
}

static int commit(hud_batch *b, hud_call *c)
{
    if (!c || !hud_call_valid(c))
        return -1;
    b->n++;
    return 0;
}

static int one_byte(hud_batch *b, uint16_t fkt, uint16_t h, uint8_t v)
{
    hud_call *c = begin(b, fkt, h);
    if (c) c->p[c->len++] = v;
    return commit(b, c);
}

int hud_set_visibility(hud_batch *b, uint16_t h, int visible)
{
    return one_byte(b, HUD_SET_VISIBILITY, h, visible ? 1 : 0);
}

int hud_switch_visibility(hud_batch *b, uint16_t h, uint16_t h2)
{
    hud_call *c = begin(b, HUD_SWITCH_VISIBILITY, h);
    if (c) {
        c->p[2] = (uint8_t)(h2 >> 8);
        c->p[3] = (uint8_t)h2;
        c->len = 4;
    }
    return commit(b, c);
}

int hud_text(hud_batch *b, uint16_t h, const char *ascii) { return hud_text_utf8(b, h, ascii); }

/* Next code point of s, BMP only; '?' for anything else. Advances *i. */
static unsigned utf8_next(const unsigned char *s, size_t *i)
{
    unsigned c = s[*i];
    if (c < 0x80) { (*i)++; return c; }
    if ((c & 0xE0) == 0xC0 && (s[*i + 1] & 0xC0) == 0x80) {
        unsigned v = (c & 0x1F) << 6 | (s[*i + 1] & 0x3F);
        *i += 2;
        return v >= 0x80 ? v : '?';
    }
    if ((c & 0xF0) == 0xE0 && (s[*i + 1] & 0xC0) == 0x80 && (s[*i + 2] & 0xC0) == 0x80) {
        unsigned v = (c & 0x0F) << 12 | (s[*i + 1] & 0x3F) << 6 | (s[*i + 2] & 0x3F);
        *i += 3;
        return v >= 0x800 && (v < 0xD800 || v > 0xDFFF) ? v : '?';
    }
    (*i)++;
    while ((s[*i] & 0xC0) == 0x80) (*i)++;
    return '?';
}

int hud_text_utf8(hud_batch *b, uint16_t h, const char *utf8)
{
    const unsigned char *s = (const unsigned char *)utf8;
    size_t i = 0, units = 0;
    hud_call *c = begin(b, HUD_TEXT, h);
    if (!c)
        return -1;
    c->p[c->len++] = 0;
    while (s[i]) {
        unsigned v = utf8_next(s, &i);
        if (++units > HUD_MAX_TEXT)
            return -1;
        c->p[c->len++] = (uint8_t)(v >> 8);
        c->p[c->len++] = (uint8_t)v;
    }
    c->p[c->len++] = 0;
    c->p[c->len++] = 0;
    return commit(b, c);
}

int hud_line_color(hud_batch *b, uint16_t h, uint8_t color) { return one_byte(b, HUD_LINE_COLOR, h, color); }
int hud_line_width(hud_batch *b, uint16_t h, uint8_t width) { return one_byte(b, HUD_LINE_WIDTH, h, width); }
int hud_fill_color(hud_batch *b, uint16_t h, uint8_t color) { return one_byte(b, HUD_FILL_COLOR, h, color); }
int hud_fill_mode(hud_batch *b, uint16_t h, uint8_t mode) { return one_byte(b, HUD_FILL_MODE, h, mode); }

int hud_polygon(hud_batch *b, uint16_t h, uint16_t id, const hud_point *pts, unsigned n)
{
    unsigned i;
    hud_call *c;
    if (n < 1 || n > HUD_MAX_POINTS)
        return -1;
    c = begin(b, HUD_POLYGON, h);
    if (!c)
        return -1;
    c->p[2] = (uint8_t)(id >> 8);
    c->p[3] = (uint8_t)id;
    c->p[4] = HUD_POLY_KIND;
    c->p[5] = (uint8_t)n;
    c->len = 6;
    for (i = 0; i < n; i++) {
        uint16_t x = (uint16_t)pts[i].x, y = (uint16_t)pts[i].y;
        c->p[c->len++] = (uint8_t)(x >> 8);
        c->p[c->len++] = (uint8_t)x;
        c->p[c->len++] = (uint8_t)(y >> 8);
        c->p[c->len++] = (uint8_t)y;
    }
    return commit(b, c);
}

int hud_remove_all(hud_batch *b, uint16_t h) { return commit(b, begin(b, HUD_REMOVE_ALL, h)); }
int hud_get_size(hud_batch *b, uint16_t h) { return commit(b, begin(b, HUD_GET_SIZE, h)); }

int hud_get_size_6wb(hud_batch *b, uint16_t h)
{
    hud_call *c = begin(b, HUD_GET_SIZE, h);
    if (!c) return -1;
    c->p[c->len++] = 1;
    return commit(b, c);
}

int hud_call_format(const hud_call *c, char *out, size_t cap)
{
    size_t used;
    unsigned i;
    int r = snprintf(out, cap, "%03X %s %u %u", c->fkt, hud_call_name(c->fkt), hud_call_handle(c), c->len);
    if (r < 0)
        return r;
    used = (size_t)r;
    for (i = 0; i < c->len; i++) {
        int k = snprintf(used < cap ? out + used : NULL, used < cap ? cap - used : 0, i ? "%02x" : " %02x", c->p[i]);
        if (k < 0)
            return k;
        used += (size_t)k;
    }
    return (int)used;
}
