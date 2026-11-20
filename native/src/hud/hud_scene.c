#include "hud_scene.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void hud_settings_default(hud_settings *s)
{
    s->sport_bar = 1;
    s->always = 0;
    s->shift_rpm = 7000;
    s->blink_rpm = 6400;
    s->in_flight = 32;   /* car run 6 W5: two overlapping frames (up to 12 calls each), 161 ms a frame, 0 errors */
    s->merge_pts = -1;
    s->cameras = 1;
    s->cam_dist = 500;   /* owner 2026-11-19: appear 500 m before */
    s->cam_orange = 0;
    s->red = 0;
}

static int take_int(const char *v, size_t n, long lo, long hi, int *out)
{
    char buf[16], *end;
    long x;
    if (n == 0 || n >= sizeof buf) return 0;
    memcpy(buf, v, n);
    buf[n] = 0;
    x = strtol(buf, &end, 10);
    if (*end || x < lo || x > hi) return 0;
    *out = (int)x;
    return 1;
}

int hud_settings_parse(hud_settings *s, const char *text, size_t len)
{
    static const struct { const char *key; size_t off; long lo, hi; } keys[] = {
        { "sport_bar", offsetof(hud_settings, sport_bar), 0, 1 },
        { "always", offsetof(hud_settings, always), 0, 1 },
        { "shift_rpm", offsetof(hud_settings, shift_rpm), 4000, 7500 },
        { "blink_rpm", offsetof(hud_settings, blink_rpm), 4000, 7500 },
        { "in_flight", offsetof(hud_settings, in_flight), 1, 32 },   /* HUD_MAX_IN_FLIGHT */
        { "merge_pts", offsetof(hud_settings, merge_pts), -1, 99 },
        { "cameras", offsetof(hud_settings, cameras), 0, 1 },
        { "cam_dist", offsetof(hud_settings, cam_dist), 100, 1500 },
        { "cam_orange", offsetof(hud_settings, cam_orange), 0, 1 },
        { "red", offsetof(hud_settings, red), 0, 255 },
    };
    size_t i = 0;
    int taken = 0;
    while (i < len) {
        size_t start = i, eq = 0, k;
        while (i < len && text[i] != '\n') {
            if (text[i] == '=' && !eq) eq = i;
            i++;
        }
        if (eq) {
            size_t end = i;
            if (end > eq + 1 && text[end - 1] == '\r') end--;
            const char *v = text + eq + 1;
            size_t vn = end - eq - 1, kn = eq - start;
            for (k = 0; k < sizeof keys / sizeof keys[0]; k++)
                if (strlen(keys[k].key) == kn && !memcmp(text + start, keys[k].key, kn) &&
                    take_int(v, vn, keys[k].lo, keys[k].hi, (int *)((char *)s + keys[k].off)))
                    taken++;
        }
        i++;
    }
    return taken;
}

int hud_settings_format(const hud_settings *s, char *out, size_t cap)
{
    return snprintf(out, cap, "sport_bar=%d\nalways=%d\nshift_rpm=%d\nblink_rpm=%d\nin_flight=%d\nmerge_pts=%d\n"
                    "cameras=%d\ncam_dist=%d\ncam_orange=%d\nred=%d\n",
                    s->sport_bar, s->always, s->shift_rpm, s->blink_rpm, s->in_flight, s->merge_pts, s->cameras,
                    s->cam_dist, s->cam_orange, s->red);
}

hud_scene hud_scene_select(const hud_settings *s, const hud_state *st)
{
    int sport = st->mode == HUD_MODE_SPORT || st->mode == HUD_MODE_SPORT_PLUS;
    if (st->factory_nav)
        return HUD_SCENE_NONE;
    return s->sport_bar && (s->always || sport) ? HUD_SCENE_RPM : HUD_SCENE_NONE;
}
