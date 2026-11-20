/* The installed app's logic, without the HMI: the menu's settings file, when the worker runs, its command line,
 * and the menu's status line. libExtendedHUD.so (extended_hud.cpp) is a thin shell around it, so all of this is
 * host-tested. Plan: .claude/stories/hud-redesign/deploy-plan.md. */
#ifndef HUD_APP_H
#define HUD_APP_H

#include "hud_scene.h"

#include <stddef.h>

/* The release (App information page). Bump both for every release; the camera list has its own date (cameras.txt). */
#define HUD_APP_VERSION "1.0.0"
#define HUD_APP_RELEASED "2026-11-20"

#define HUD_APP_DIR "/var/opt/hmi/data/ID5/Plugins/ExtendedHUD"
#define HUD_APP_PLUGINS "/var/opt/hmi/data/ID5/Plugins"
enum { HUD_APP_UDP_PORT = 42873, HUD_APP_MAX_ARGS = 40 };

typedef enum { HUD_APP_SPORT_BAR, HUD_APP_ALWAYS, HUD_APP_CAMERAS, HUD_APP_CAM_ORANGE } hud_app_item;

/* The installed defaults: the driving mode is not wired yet (deploy plan, version 1), so "Always" starts ticked or
   the Sport Bar would never show. */
void hud_app_defaults(hud_settings *s);
/* Reads dir/settings.txt over the defaults; a missing or bad file leaves the defaults. */
void hud_app_load(hud_settings *s, const char *dir);
/* Writes dir/settings.txt through a temporary file and a rename, so the worker never reads half a file. 0 or -1. */
int hud_app_save(const hud_settings *s, const char *dir);
/* Flips one menu item. "Always" only while Sport Bar is ticked (owner, 2026-11-18: it greys out), the orange colour only
   while the camera warning is. */
void hud_app_toggle(hud_settings *s, hud_app_item item);
/* The Shift point page's values (owner 2026-11-18), in menu order. */
enum { HUD_APP_BLINK_CHOICES = 5 };
extern const int hud_app_blink_rpm[HUD_APP_BLINK_CHOICES];
/* Takes choice i (0..4) of the Shift point page; anything else leaves the setting. */
void hud_app_set_blink(hud_settings *s, int i);
/* The Shift point page's label, e.g. "Gear blinks from 6,400 RPM". */
void hud_app_blink_label(const hud_settings *s, char *out, size_t cap);
/* The Warning distance page's values (owner 2026-11-20), in menu order. */
enum { HUD_APP_CAM_DIST_CHOICES = 3 };
extern const int hud_app_cam_dist_m[HUD_APP_CAM_DIST_CHOICES];
/* Takes choice i (0..2) of the Warning distance page; anything else leaves the setting. */
void hud_app_set_cam_dist(hud_settings *s, int i);
/* The Warning distance page's label, e.g. "Warning from 500 m before the camera". */
void hud_app_cam_dist_label(const hud_settings *s, char *out, size_t cap);
/* The worker runs (and polls the engine) only while something is ticked. */
int hud_app_wants_worker(const hud_settings *s);
/* Another HUD sender installed next to ours (two senders freeze the HUD: home repo #22/#27): its folder name, or NULL. */
const char *hud_app_conflict(const char *plugins_dir);
/* The worker's command line, the split Sport Bar (approved 2026-11-18). argv points into buf. Returns argc, or
   -1 if it does not fit. */
int hud_app_worker_argv(const char *dir, const char *gateway, char *buf, size_t cap, const char **argv, int max);
/* The App information page: "Version 1.0.0, released 2026-11-20" and, from dir/cameras.txt (its "# country NAME" and
   "# date YYYY-MM-DD" lines, written by tools/cameras_build.py and updated without a release: skill update-speedcam-db),
   "Speed cameras: Ukraine, 426, data from 2026-11-19". A missing file, country or date says so. */
typedef struct {
    int count;                  /* camera lines */
    char country[32];           /* "" when the file names none */
    char date[11];              /* YYYY-MM-DD, "" when none */
} hud_cams_meta;
void hud_app_version_label(char *out, size_t cap);
void hud_app_cameras_label(const char *dir, char *out, size_t cap);
/* One cameras.txt line (no newline needed) into m; start from a zeroed m. */
void hud_app_cameras_line(hud_cams_meta *m, const char *line);
/* The menu's status line. */
void hud_app_status(const hud_settings *s, int running, const char *conflict, int restarts, char *out, size_t cap);

#endif
