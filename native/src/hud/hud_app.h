/* The installed app's logic, without the HMI: the menu's settings file, when the worker runs, its command line,
 * and the menu's status line. libExtendedHUD.so (extended_hud.cpp) is a thin shell around it, so all of this is
 * host-tested. Plan: .claude/stories/hud-redesign/deploy-plan.md. */
#ifndef HUD_APP_H
#define HUD_APP_H

#include "hud_scene.h"

#include <stddef.h>

#define HUD_APP_DIR "/var/opt/hmi/data/ID5/Plugins/ExtendedHUD"
#define HUD_APP_PLUGINS "/var/opt/hmi/data/ID5/Plugins"
enum { HUD_APP_UDP_PORT = 42873, HUD_APP_MAX_ARGS = 40 };

typedef enum { HUD_APP_SPORT_BAR, HUD_APP_ALWAYS, HUD_APP_NAV } hud_app_item;

/* The installed defaults: the driving mode is not wired yet (deploy plan, version 1), so "Always" starts ticked or
   the Sport Bar would never show. */
void hud_app_defaults(hud_settings *s);
/* Reads dir/settings.txt over the defaults; a missing or bad file leaves the defaults. */
void hud_app_load(hud_settings *s, const char *dir);
/* Writes dir/settings.txt through a temporary file and a rename, so the worker never reads half a file. 0 or -1. */
int hud_app_save(const hud_settings *s, const char *dir);
/* Flips one menu item. "Always" only while Sport Bar is ticked (owner, 2026-11-18: it greys out). */
void hud_app_toggle(hud_settings *s, hud_app_item item);
/* The worker runs (and polls the engine) only while something is ticked. */
int hud_app_wants_worker(const hud_settings *s);
/* Another HUD sender installed next to ours (two senders freeze the HUD: home repo #22/#27): its folder name, or NULL. */
const char *hud_app_conflict(const char *plugins_dir);
/* The worker's command line, the run-4 design (car runs T2 + T3, frozen 2026-11-18). argv points into buf. Returns argc, or
   -1 if it does not fit. */
int hud_app_worker_argv(const char *dir, const char *gateway, char *buf, size_t cap, const char **argv, int max);
/* The menu's status line. */
void hud_app_status(const hud_settings *s, int running, const char *conflict, int restarts, char *out, size_t cap);

#endif
