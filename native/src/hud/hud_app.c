#define _POSIX_C_SOURCE 200809L
#include "hud_app.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

void hud_app_defaults(hud_settings *s)
{
    hud_settings_default(s);
    s->sport_bar = 1;
    s->always = 1;
    s->cameras = 1;
}

void hud_app_load(hud_settings *s, const char *dir)
{
    char path[256], buf[512];
    size_t n;
    FILE *f;
    hud_app_defaults(s);
    snprintf(path, sizeof path, "%s/settings.txt", dir);
    if (!(f = fopen(path, "r"))) return;
    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    hud_settings_parse(s, buf, n);
}

int hud_app_save(const hud_settings *s, const char *dir)
{
    char path[256], tmp[260], buf[512];
    int n = hud_settings_format(s, buf, sizeof buf), ok;
    FILE *f;
    if (n <= 0 || (size_t)n >= sizeof buf) return -1;
    snprintf(path, sizeof path, "%s/settings.txt", dir);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    if (!(f = fopen(tmp, "w"))) return -1;
    ok = fwrite(buf, 1, (size_t)n, f) == (size_t)n;
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, path) != 0) {
        remove(tmp);
        return -1;
    }
    return 0;
}

void hud_app_toggle(hud_settings *s, hud_app_item item)
{
    int *v;
    if (item == HUD_APP_ALWAYS && !s->sport_bar) return;   /* greyed out in the menu while Sport Bar is off */
    if (item == HUD_APP_CAM_ORANGE && !s->cameras) return;   /* and the colour while the camera warning is off */
    v = item == HUD_APP_SPORT_BAR ? &s->sport_bar : item == HUD_APP_ALWAYS ? &s->always
        : item == HUD_APP_CAMERAS ? &s->cameras : &s->cam_orange;
    *v = !*v;
}

const int hud_app_blink_rpm[HUD_APP_BLINK_CHOICES] = { 6200, 6300, 6400, 6500, 6600 };

void hud_app_set_blink(hud_settings *s, int i)
{
    if (i >= 0 && i < HUD_APP_BLINK_CHOICES) s->blink_rpm = hud_app_blink_rpm[i];
}

void hud_app_blink_label(const hud_settings *s, char *out, size_t cap)
{
    snprintf(out, cap, "Gear blinks from %d,%03d RPM", s->blink_rpm / 1000, s->blink_rpm % 1000);
}

const int hud_app_cam_dist_m[HUD_APP_CAM_DIST_CHOICES] = { 300, 500, 800 };

void hud_app_set_cam_dist(hud_settings *s, int i)
{
    if (i >= 0 && i < HUD_APP_CAM_DIST_CHOICES) s->cam_dist = hud_app_cam_dist_m[i];
}

void hud_app_cam_dist_label(const hud_settings *s, char *out, size_t cap)
{
    snprintf(out, cap, "Warning from %d m before the camera", s->cam_dist);
}

int hud_app_wants_worker(const hud_settings *s) { return s->sport_bar || s->cameras; }

const char *hud_app_conflict(const char *plugins_dir)
{
    /* the packaged HUD Gauges (connector 107); others get added here when we meet them */
    static const char *const k_others[] = { "HudTest" };
    char path[256];
    size_t i;
    for (i = 0; i < sizeof k_others / sizeof k_others[0]; i++) {
        snprintf(path, sizeof path, "%s/%s", plugins_dir, k_others[i]);
        if (access(path, F_OK) == 0) return k_others[i];
    }
    return NULL;
}

int hud_app_worker_argv(const char *dir, const char *gateway, char *buf, size_t cap, const char **argv, int max)
{
    /* The split Sport Bar (owner, 2026-11-18): the whole track dim (3), the lit part white (4) like the factory speed,
       the gear left (the gearbox's, D/S only) and the RPM right, merged into ~7 calls a frame. The latency knobs
       (in_flight, merge_pts) come from settings.txt, so car runs tune them without a new build. */
    static const char *const k_fixed[] = {
        "--draw-live", "--layout", "split", "--flash-ms", "0", "--palette", "3,4,3,4,3,4,4", "--shift-colour", "4",
        "--backs", "fill", "--most-inst", "2", "--handles", "31,32", "--canvas", "120x72", "--container", "23",
        "--udp-port", "42873",
        /* car run 6 W5: frames overlap (the next starts once the last is all sent), 6.2 frames/s against 3.4 */
        "--pipeline",
    };
    const char *paths[4];
    size_t used = 0, i;
    int argc = 0;
    char *p = buf;
    /* cameras.txt: the speed-camera list (tools/cameras_build.py); none = no camera alerts */
    const char *fmt[4] = { "%s/hud-sportbar-qnx", "%s/shift-deck-qnx", "%s/settings.txt", "%s/cameras.txt" };
    for (i = 0; i < 4; i++) {
        int n = snprintf(p, cap - used, fmt[i], dir);
        if (n < 0 || (size_t)n >= cap - used) return -1;
        paths[i] = p;
        p += n + 1;
        used += (size_t)n + 1;
    }
    if (max < (int)(sizeof k_fixed / sizeof k_fixed[0]) + 10) return -1;
    argv[argc++] = paths[0];
    for (i = 0; i < sizeof k_fixed / sizeof k_fixed[0]; i++) argv[argc++] = k_fixed[i];
    argv[argc++] = "--collector";
    argv[argc++] = paths[1];
    argv[argc++] = "--host";
    argv[argc++] = gateway;
    argv[argc++] = "--settings";
    argv[argc++] = paths[2];
    argv[argc++] = "--cameras";
    argv[argc++] = paths[3];
    argv[argc] = NULL;
    return argc;
}

void hud_app_status(const hud_settings *s, int running, const char *conflict, int restarts, char *out, size_t cap)
{
    if (conflict) snprintf(out, cap, "Off: remove %s first (two HUD apps freeze the HUD)", conflict);
    else if (!hud_app_wants_worker(s)) snprintf(out, cap, "Off");
    else if (!running) snprintf(out, cap, restarts ? "Stopped, retrying (%d)" : "Starting", restarts);
    else if (!s->sport_bar) snprintf(out, cap, "Running: camera warning only");
    else if (!s->always) snprintf(out, cap, s->cameras ? "Running: bar in Sport and Sport+, camera warning"
                                                       : "Running: bar in Sport and Sport+");
    else snprintf(out, cap, s->cameras ? "Running: bar and camera warning" : "Running: bar");
}
