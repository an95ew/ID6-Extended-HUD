/* Speed-camera alert (owner 2026-11-19, story speed-cameras): the fixed camera ahead on the driver's side, shown from 500 m
 * before it until 50 m after it, with that camera's speed limit. The list is the Patrol Police table of fixed cameras, made
 * into cameras.txt by tools/cameras_build.py; the position is the car's GPS from CDS. Pure code: no I/O, no clocks. */
#ifndef HUD_CAM_H
#define HUD_CAM_H

#include "hud_proto.h"
#include "hud_strip.h"

#include <stddef.h>
#include <stdint.h>

enum {
    HUD_CAM_MAX = 1024,
    HUD_CAM_SHOW_M = 500,       /* appears this far before the camera */
    HUD_CAM_PAST_M = 50,        /* and goes this far after it */
    HUD_CAM_AHEAD_DEG = 30,     /* the camera must lie within this of the heading to count as ahead */
    HUD_CAM_DIR_DEG = 50,       /* the heading within this of the direction the camera watches */
    HUD_CAM_SIDE_M = 60,        /* farther off the line of travel than this: not on our road, or we turned off */
    HUD_CAM_HEADING_MOVE_M = 12 /* without a CDS heading, take it from two fixes at least this far apart */
};

typedef struct {
    int32_t lat_e6, lon_e6;     /* WGS84 degrees x 1e6 */
    int16_t limit;              /* km/h, 0 unknown */
    int16_t dir;                /* the direction of travel it watches, degrees 0..359 from north, -1 unknown (then both) */
} hud_cam;

/* cameras.txt: "lat lon limit dir" per line, decimal degrees, '#' comments. Bad lines are skipped. Returns the count. */
int hud_cams_parse(const char *text, size_t len, hud_cam *out, int max);

typedef struct {
    int32_t lat_e6, lon_e6;
    int heading;                /* degrees 0..359, -1 unknown */
    int valid;
} hud_geo_fix;

typedef struct {
    int active, idx;            /* the camera shown, while active */
    int dist_m;                 /* to the camera along the heading; negative once passed */
    int limit;
    /* private */
    hud_geo_fix last, anchor;   /* anchor: the fix the derived heading is measured from */
    int heading;
} hud_cam_track;

void hud_cam_track_init(hud_cam_track *t);
/* A new fix (or an invalid one when the position is stale: the alert ends). Returns 1 when what is shown changed. */
int hud_cam_track_update(hud_cam_track *t, const hud_cam *cams, int n, const hud_geo_fix *fix);

/* Metres east/north of b from a, flat earth (fine within a few km). */
void hud_geo_offset(int32_t lat_a, int32_t lon_a, int32_t lat_b, int32_t lon_b, double *east, double *north);

/* The official camera sign's camera (body with two lens rings, stepped lines, mount), as polygons at (x, y):
   badge 0 is 48 x 36 (the whole-picture alert), badge 1 is 28 x 24 (the gap between gear and revs). Returns the count. */
enum { HUD_CAM_ICON_POLYS = 8, HUD_CAM_ICON_PTS = 64 };
int hud_cam_icon(int badge, int x, int y, hud_point pts[][HUD_CAM_ICON_PTS], unsigned *npts);

/* The whole picture: the camera in red_ink, a limit sign (ring and digits) and a distance bar in the ink. Draws into the
   hidden image and switches, as hud_strip_frame does. Returns the calls added, -1 if they would not fit. */
int hud_cam_frame(hud_batch *b, const hud_palette *pal, uint8_t red_ink, const hud_handles *hh, int limit, int dist_m,
                  int *shown);

#endif
