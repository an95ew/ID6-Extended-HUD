/* What the right half of the HUD shows, from the menu settings and the car's state.
 * Rules: .claude/stories/hud-redesign/menu-spec.md. */
#ifndef HUD_SCENE_H
#define HUD_SCENE_H

#include <stddef.h>
#include <stdint.h>

typedef enum { HUD_SCENE_NONE, HUD_SCENE_RPM } hud_scene;

typedef enum { HUD_MODE_UNKNOWN, HUD_MODE_COMFORT, HUD_MODE_SPORT, HUD_MODE_SPORT_PLUS, HUD_MODE_ECO_PRO } hud_drive_mode;

typedef struct {
    int sport_bar;        /* menu: Sport Bar */
    int always;           /* menu: Always show Sport Bar */
    int shift_rpm;        /* where the bar ends (7000, the redline); no menu row */
    int blink_rpm;        /* menu: Shift point. The gear blinks from here (owner 2026-11-18: 6200..6600, default 6400) */
    int in_flight;        /* HUD calls awaiting an answer, 1..32 (the factory's 3); tuned on the car, no menu row */
    int merge_pts;        /* points per merged polygon, 0 = one per outline, -1 = the layout's; tuned on the car */
    int cameras;          /* menu: Display Speed Camera Warning; needs cameras.txt and the GPS from CDS */
    int cam_dist;         /* menu: Warning distance, metres before the camera (owner 2026-11-20: 300, 500, 800) */
    int cam_orange;       /* menu: Orange camera warning: the camera and its limit in orange (fill 5), else white */
    int red;              /* the HUD's red as a fill index, 0 = none known (owner 2026-11-19: gear blink, 6000+ zone, camera) */
} hud_settings;

typedef struct {
    hud_drive_mode mode;
    int factory_nav;      /* the factory navigation owns the HUD */
} hud_state;

void hud_settings_default(hud_settings *s);
/* "key=value" lines; unknown keys and bad values are ignored. Returns the number of keys taken. */
int hud_settings_parse(hud_settings *s, const char *text, size_t len);
int hud_settings_format(const hud_settings *s, char *out, size_t cap);

/* With the Sport Bar ticked, Sport and Sport+ show it (Always: every mode). The scene changes only with the menu, the
   driving mode and the factory navigation, so it cannot flicker. */
hud_scene hud_scene_select(const hud_settings *s, const hud_state *st);

#endif
