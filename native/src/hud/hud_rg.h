/* Required Notice: Copyright 2026 nbtevo-carplay-advanced contributors
 * Licensed under the PolyForm Noncommercial License 1.0.0 (https://polyformproject.org/licenses/noncommercial/1.0.0)
 *
 * CarPlay route guidance for the nav scene: the routeguidance file librghook.so writes inside mm-ipod
 * (src/rghook/rghook.c, default /var/iap/pps/routeguidance), read into hud_nav_info. The file format and its
 * meaning follow st1ng/nbtevo-carplay-advanced (src/rghook.c writes it, src/hud.h reads it). PPS-style lines:
 *
 *   @routeguidance
 *   route_state:n:1                   iAP2 RouteGuidanceState: 0 no route, 1 active, 2 arrived, 3 loading,
 *                                     4 locating, 5 rerouting, 6 proceed to route
 *   dist_to_maneuver:n:450            metres
 *   dist_to_maneuver_units:n:2        units of the phone's text: 0 km, 1 mi, 2 m, 3 yd, 4 ft
 *   dist_to_maneuver_string::450      the phone's distance text
 *   current_road::Main St
 *   maneuver_index:n:3                present only while the phone lists a maneuver
 *   maneuver_type:n:2                 iAP2 / CPManeuverType 0..53
 *   maneuver_exit_angle:n:-90         0 straight on, positive to the right
 *   maneuver_driving_side:n:0         0 right-hand traffic, 1 left-hand
 *   maneuver_road::Oak Ave
 *   lanes::0/1000/0;2/90/0,90         left to right: status/highlight/angle,angle (status 0 not good, 1 good,
 *                                     2 preferred; highlight 1000 = none)
 *
 * Pure code but the poller, so the host tests cover it. */
#ifndef HUD_RG_H
#define HUD_RG_H

#include "hud_nav.h"

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define HUD_RG_FILE "/var/iap/pps/routeguidance"

enum { HUD_RG_DIST_TEXT = 32 };

typedef struct {
    int route_state;      /* -1: no route_state line */
    int has_maneuver;     /* maneuver_index was there */
    int active;           /* a maneuver on an active route (route_state 1, 5, 6) */
    int hold;             /* loading / locating (3, 4): keep the last picture */
    unsigned index, type; /* maneuver index, iAP2 maneuver type */
    int angle, side;      /* exit angle as the phone sends it, driving side (1 = left-hand traffic) */
    int units;            /* dist_to_maneuver_units, -1: none */
    char dist_text[HUD_RG_DIST_TEXT];   /* the phone's distance with its unit ("450 m", "0.3 mi"), "" = none */
    hud_nav_info nav;     /* man, exit, exit_angle, dist_m, street, lanes; total_m = dist_m (see hud_rg_track) */
} hud_rg;

/* The file's text (need not be NUL-terminated) into r. Returns 0, or -1 when it is not a routeguidance file. */
int hud_rg_parse(hud_rg *r, const char *text, size_t len);

/* iAP2 maneuver type, exit angle and driving side -> our manoeuvre; *exit (may be NULL) gets the roundabout exit
   number of types 28..46 (exit 1..19), else 0. Unknown and angle-only types (0 no turn, 3 straight, 5 follow road)
   go by the angle. */
enum hud_maneuver hud_rg_maneuver(unsigned type, int angle, int side, int *exit);
/* The phone's exit angle (0 straight, +90 right) -> ours on a roundabout (degrees round it, 90 = first right,
   180 = straight over); 0 stays 0 (hud_nav then goes by the exit number). */
int hud_rg_roundabout_angle(int angle);
/* A lane's ways out (degrees as the exit angle) -> HUD_LANE_* bits. */
unsigned hud_rg_lane_arrows(const int *ang, int n);

/* What the worker keeps between reads: the route and the distance the bar runs from. */
typedef struct {
    int route_active;     /* the nav scene may show */
    unsigned index;       /* the maneuver total_m belongs to */
    int total_m, have;
    hud_nav_info nav;     /* the last guidance (kept while the phone is loading / locating) */
} hud_rg_track;

void hud_rg_track_init(hud_rg_track *t);
/* A fresh read (NULL: no file, i.e. no route). Sets t->route_active and t->nav (total_m from the first distance
   seen for this maneuver). Returns 1 when either changed. */
int hud_rg_track_update(hud_rg_track *t, const hud_rg *r);

/* Polls the file: rereads only when it changed (inode, size, mtime: librghook replaces it by rename). */
typedef struct {
    const char *path;
    int present;
    long long ino, size;
    time_t mtime;
    hud_rg rg;
} hud_rg_file;

void hud_rg_file_init(hud_rg_file *f, const char *path);
/* 1: changed (f->present says whether there is a file, f->rg holds it), 0: unchanged. */
int hud_rg_file_poll(hud_rg_file *f);

#endif
