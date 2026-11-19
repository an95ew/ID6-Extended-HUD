/* HUD terminal calls on MOST FBlock 0x82/00, OpType 6. Layouts and limits follow the packaged
 * hud-shape-qnx worker; see .claude/stories/hud-redesign/hudterm-protocol.md for where each comes from.
 * Every payload starts with the 2-byte big-endian handle of the instance it draws on. */
#ifndef HUD_PROTO_H
#define HUD_PROTO_H

#include <stddef.h>
#include <stdint.h>

#define HUD_FBLOCK 0x82
#define HUD_INST   0x00
#define HUD_OPTYPE 6                /* StartResultAck */

enum {
    HUD_SET_VISIBILITY    = 0x204,
    HUD_SWITCH_VISIBILITY = 0x20E,
    HUD_TEXT              = 0x210,
    HUD_LINE_COLOR        = 0x218,
    HUD_LINE_WIDTH        = 0x219,
    HUD_FILL_COLOR        = 0x21B,
    HUD_FILL_MODE         = 0x21C,
    HUD_POLYGON           = 0x21D,
    HUD_CIRCLE            = 0x21E,
    HUD_REMOVE_ALL        = 0x220,
    HUD_GET_SIZE          = 0xC04,
    HUD_CIRCLE_DIAMETER   = 0xC09
};

enum {
    HUD_MAX_POINTS  = 99,
    HUD_MAX_TEXT    = 32,           /* UTF-16 units before the terminator */
    HUD_MAX_PAYLOAD = 6 + 4 * HUD_MAX_POINTS,
    HUD_MAX_CALLS   = 48,           /* per batch */
    HUD_IN_FLIGHT   = 3,            /* calls awaiting an answer: the factory sender's window */
    HUD_MAX_IN_FLIGHT = 32,         /* the most --in-flight takes (car run 6: 12 in flight answered cleanly; 32 untested) */
    HUD_POLY_KIND   = 3             /* the kind tachBand uses; 2 is also accepted */
};

typedef struct { int16_t x, y; } hud_point;

typedef struct {
    uint16_t fkt;
    uint16_t len;
    uint8_t p[HUD_MAX_PAYLOAD];
} hud_call;

typedef struct {
    unsigned n;
    hud_call call[HUD_MAX_CALLS];
} hud_batch;

/* 1 when the call passes the same checks the packaged worker applies before sending. */
int hud_call_valid(const hud_call *c);
/* The handle the answer is matched on (first two payload bytes). */
uint16_t hud_call_handle(const hud_call *c);
const char *hud_call_name(uint16_t fkt);

void hud_batch_init(hud_batch *b);
/* Each adder returns 0, or -1 when the batch is full or the call would be refused; the batch is then
   unchanged. */
int hud_set_visibility(hud_batch *b, uint16_t h, int visible);
int hud_switch_visibility(hud_batch *b, uint16_t h, uint16_t h2);
int hud_text(hud_batch *b, uint16_t h, const char *ascii);
/* UTF-8 in, UTF-16BE out. Characters outside the BMP and bad bytes become '?'. -1 over 32 units. */
int hud_text_utf8(hud_batch *b, uint16_t h, const char *utf8);
int hud_line_color(hud_batch *b, uint16_t h, uint8_t color);
int hud_line_width(hud_batch *b, uint16_t h, uint8_t width);
int hud_fill_color(hud_batch *b, uint16_t h, uint8_t color);
int hud_fill_mode(hud_batch *b, uint16_t h, uint8_t mode);
int hud_polygon(hud_batch *b, uint16_t h, uint16_t id, const hud_point *pts, unsigned n);
int hud_remove_all(hud_batch *b, uint16_t h);
int hud_get_size(hud_batch *b, uint16_t h);
/* The 6WB cluster's form, [h][01]; it answers [h][01][w u16][h u16] (JayJay3071/nbt-android-auto). The F54 answers
   ErrorAck "invalid length" to it and takes plain [h] (st1ng/nbtevo-carplay-advanced). */
int hud_get_size_6wb(hud_batch *b, uint16_t h);

/* One line per call: "fkt name handle len hex…". Returns the bytes written, like snprintf. */
int hud_call_format(const hud_call *c, char *out, size_t cap);

#endif
