/* The M Strip: twelve slanted RPM fields on the right half of the HUD with M HUD colours and logic, drawn
 * double-buffered (clear the hidden image, draw, switch). Pure code: no I/O, no clocks of its own. */
#ifndef HUD_STRIP_H
#define HUD_STRIP_H

#include "hud_proto.h"

#include <stdint.h>

enum { HUD_FIELDS = 12 };

/* Field zones in RPM. Fields 1-6 cover lo..yellow, 7-9 yellow..red, 10-12 red..shift. The bar flashes from
   shift on. B58 default (owner, 2026-11-18): the bar ends at the 7000 redline, so 1000-5000 low, 5000-6000
   middle and 6000-7000 tallest. flash_ms is the half period of that flash;
   0 holds it steady (lit), since the car draws one frame per ~1.5 s (run 2). */
typedef struct { int lo, yellow, red, shift, redline, flash_ms; } hud_profile;

/* Palette indices on the HUD. They are not RGB, and which index is which colour is not known yet: the
   defaults are placeholders until the swatch test in the car. */
enum { HUD_BACKS_FILL, HUD_BACKS_NONE, HUD_BACKS_OUTLINE };
/* shift_on (0 = the zone colours) lights the whole bar in one colour from the shift point: the HUD has no red, so the
   cue is dim -> bright (run 3: 3 looks red-orange at night, 4 white). backs: how unlit fields show; NONE when the lit
   colour is the unlit one, OUTLINE draws them in line colour base_off with fill 0 (untested on the car). */
typedef struct { uint8_t base_off, base_on, yellow_off, yellow_on, red_off, red_on, ink, shift_on; int backs; } hud_palette;

/* Layout in HUD canvas units. Defaults come from the design gallery's 480x180 canvas. */
typedef struct {
    int canvas_w, canvas_h;
    int x, y, w, h;                  /* the strip */
    int gap, slant;
    int gear_cx, gear_top, gear_h;   /* 7-segment gear digit; gear_h 0 hides it */
    int nav_x0;                      /* the nav scene's design x (of 480) at canvas x 0: 0, or 240 for the big layout */
    int nav_text_top;                /* big layout: the HUD's own 0x1A/0x1B texts sit over the image's top (run 4 T3), keep it free */
    int rise;                        /* red fields stand this much taller, yellow half: the HUD has no red (run 3) */
    int rpm_x1, rpm_h;               /* RPM readout, right-aligned at rpm_x1, bottom on the gear's; rpm_h 0 hides it */
    int solid_digits;                /* 1: each digit is one polygon (holes as keyholes), 0: one polygon per segment */
} hud_geom;

typedef struct {
    uint16_t buf[2];                 /* the two images that swap (5 and 6 in the packaged worker) */
    uint16_t static_h;               /* a third image for the unlit fields; none is known yet (2-4 are text) */
    int static_backs;
} hud_handles;

typedef struct {
    double rpm;                      /* eased display RPM */
    double peak;
    uint32_t peak_ms;
    uint32_t now_ms;
    int have;
    int gear;                        /* 0 hides it */
} hud_anim;

/* What a frame looks like; equal keys draw identical frames, so the frame can be skipped. */
typedef struct { int full, partial_px, flash_on, shifting, peak_field, gear, rpm; } hud_strip_key;

void hud_profile_b58(hud_profile *p);
/* Moves the zones with the bar's end (the shift point): yellow 2000 below it, red 1000 below it. */
void hud_profile_for_shift(hud_profile *p, int shift);
void hud_palette_default(hud_palette *p);
void hud_geom_default(hud_geom *g);
/* Scales the default layout to a canvas reported by ImageGetSize. */
void hud_geom_for_canvas(hud_geom *g, int canvas_w, int canvas_h);
/* The whole canvas: the strip across the top, the gear digit below it. For the small HUD image (82/02 31/32 is
   120x72), where the scaled default uses only a sixth of it. */
void hud_geom_big(hud_geom *g, int canvas_w, int canvas_h);
/* The split layout (2026-11-18): the strip as in the big one, under it the gear on the left and the RPM on the right,
   every digit one polygon, so the readout costs a call per digit. */
void hud_geom_split(hud_geom *g, int canvas_w, int canvas_h);
void hud_handles_default(hud_handles *h);
/* The RPM the readout shows: to the nearest 50, as the car draws one frame per ~2 s. */
int hud_rpm_shown(double rpm);
/* Digit d (0..9) as one polygon in a w x h box at (x, y), stroke t: holes are keyholes on a horizontal bridge, which
   adds no edge a scanline crosses. Returns the point count (at most HUD_DIGIT_PTS). */
enum { HUD_DIGIT_PTS = 18 };
int hud_digit_poly(int d, int x, int y, int w, int h, int t, hud_point *pt);

/* Field i's RPM span. */
void hud_field_span(const hud_profile *p, int i, double *lo, double *hi);
/* 0 base, 1 yellow, 2 red. */
int hud_field_zone(int i);

void hud_anim_reset(hud_anim *a);
/* Eases toward raw_rpm (45 ms time constant), holds the peak 400 ms then drops it at 6000 rpm/s. */
void hud_anim_step(hud_anim *a, double raw_rpm, uint32_t now_ms);
int hud_anim_shifting(const hud_profile *p, const hud_anim *a);
/* 4 Hz flash at the shift point: on for 125 ms, off for 125 ms. */
int hud_anim_flash_on(const hud_anim *a);
/* The flash with the profile's half period (flash_ms; 0 = steady on). */
int hud_anim_flash_on_p(const hud_profile *p, const hud_anim *a);

hud_strip_key hud_strip_key_of(const hud_profile *p, const hud_geom *g, const hud_anim *a);
int hud_strip_key_equal(const hud_strip_key *a, const hud_strip_key *b);

/* Draws one frame into the hidden image buf[1 - *shown] and switches to it; *shown flips. Returns the
   number of calls added, or -1 when they would not fit (the batch is then left as it was). */
int hud_strip_frame(hud_batch *b, const hud_profile *p, const hud_palette *pal, const hud_geom *g,
                    const hud_handles *h, const hud_anim *a, int *shown);
/* Unlit fields, once, on static_h. */
int hud_strip_static(hud_batch *b, const hud_profile *p, const hud_palette *pal, const hud_geom *g,
                     const hud_handles *h);
/* Clears everything we drew. */
int hud_strip_clear(hud_batch *b, const hud_handles *h);
/* Car test: 16 squares with fill colours first..first+15, 8 per row, on image h. */
int hud_swatches(hud_batch *b, uint16_t h, const hud_geom *g, uint8_t first);
/* Car test for a small image: n bars (1..8) over the whole canvas with colours first..first+n-1, each with a white
   (index tick) foot so a bar that draws nothing still has its place. line != 0 tests the line colour instead: fill 0
   (draws nothing on 82/00) and a 3 px outline in the index. */
int hud_swatch_bars(hud_batch *b, uint16_t h, const hud_geom *g, uint8_t first, int n, int line, uint8_t tick);
/* 32 markers over the whole canvas, 8 columns by 4 rows, each column 1 px wider and each row 2 px taller than the
   one before: a photo of the HUD shows which part of the image is visible, and where. */
int hud_grid(hud_batch *b, uint16_t h, const hud_geom *g, uint8_t colour);

/* Gear from RPM and speed: the nearest ratio within 12%, or 0. */
int hud_gear_estimate(double rpm, double kph);

#endif
