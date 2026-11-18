/* The navigation scene: manoeuvre arrow, distance bar and lanes drawn on the double-buffered images; street
 * name and distance written to text areas. Right-hand traffic (Ukraine): U-turns go left, roundabouts run
 * anticlockwise. Pure code. */
#ifndef HUD_NAV_H
#define HUD_NAV_H

#include "hud_strip.h"
#include "hud_text.h"

#include <stddef.h>

enum hud_maneuver {
    HUD_MAN_STRAIGHT, HUD_MAN_LEFT, HUD_MAN_RIGHT, HUD_MAN_SLIGHT_LEFT, HUD_MAN_SLIGHT_RIGHT,
    HUD_MAN_UTURN, HUD_MAN_ROUNDABOUT, HUD_MAN_ARRIVE,
    HUD_MAN_SHARP_LEFT, HUD_MAN_SHARP_RIGHT,
    HUD_MAN_KEEP_LEFT, HUD_MAN_KEEP_RIGHT,     /* a fork: the other branch grey */
    HUD_MAN_EXIT_LEFT, HUD_MAN_EXIT_RIGHT,     /* an off-ramp: the main road grey */
    HUD_MAN_MERGE, HUD_MAN_DEPART,
    HUD_MAN_COUNT
};

/* Lane arrows, any combination. */
enum { HUD_LANE_STRAIGHT = 1, HUD_LANE_LEFT = 2, HUD_LANE_RIGHT = 4, HUD_LANE_SLIGHT_LEFT = 8,
       HUD_LANE_SLIGHT_RIGHT = 16, HUD_LANE_UTURN = 32 };
/* Lane kinds: a bus lane carries the letter А, a tram lane its rails. */
enum { HUD_LANE_CAR = 0, HUD_LANE_BUS = 1, HUD_LANE_TRAM = 2 };

enum { HUD_MAX_LANES = 6, HUD_STREET_MAX = 256 };

typedef struct {
    uint8_t arrows;       /* what the lane allows */
    uint8_t kind;
    uint8_t recommended;  /* a lane to take */
    uint8_t use;          /* of its arrows, the ones the route takes (white; the rest grey). 0: all of them */
} hud_lane;

typedef struct {
    enum hud_maneuver man;
    int exit;                        /* roundabout exit, 1 = first right */
    int exit_angle;                  /* roundabout exit angle in degrees, 90 = right; 0: from exit */
    int dist_m;
    int total_m;                     /* distance when the manoeuvre was announced; the bar runs from it */
    char street[HUD_STREET_MAX];     /* UTF-8 as the phone sends it */
    hud_lane lane[HUD_MAX_LANES];
    int nlanes;
} hud_nav_info;

/* Text areas. The packaged worker's profile lists 2, 3 and 4 as text areas and writes its own text to 3.
   Where each sits on the HUD, its font and whether it has Cyrillic are set by the cluster: unverified. */
typedef struct { uint16_t street, distance; } hud_text_handles;

/* What is already on the HUD, so a frame sends only what changed. */
typedef struct {
    char street[HUD_TEXT_BYTES], distance[HUD_TEXT_BYTES];   /* as sent: cleaned and cut */
    int have_text;
    int man, exit, exit_angle, bar_px, nlanes;
    hud_lane lane[HUD_MAX_LANES];
    int have_image;
} hud_nav_sent;

void hud_text_handles_default(hud_text_handles *t);
/* "450 м", "1,2 км", "Ви прибули" in the style's language and script (NULL: Ukrainian), UTF-8. */
void hud_nav_distance_text(int dist_m, enum hud_maneuver man, const hud_text_style *ts, char *out, size_t cap);
/* Texts that changed (cleaned and cut by hud_text_street), then the image if it changed (clear, draw, switch;
   *shown flips). Every polygon is a simple outline (exact line and arc offsets). When the lanes would not fit
   the batch, separators, then lane markers, then the lanes are left out. Returns the calls added (0 when
   nothing changed), or -1 when they would not fit (the batch is then left as it was). ts NULL: defaults. */
int hud_nav_frame(hud_batch *b, const hud_nav_info *n, const hud_palette *pal, const hud_geom *g,
                  const hud_handles *h, const hud_text_handles *th, const hud_text_style *ts,
                  hud_nav_sent *sent, int *shown);
/* Empties the texts we wrote and forgets what was sent. */
int hud_nav_clear(hud_batch *b, const hud_text_handles *th, hud_nav_sent *sent);

#endif
