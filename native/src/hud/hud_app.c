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
    s->nav = 1;
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
    if (item == HUD_APP_ALWAYS && !s->sport_bar) return;   /* greyed out in the menu while Sport Bar is off */
    {
    int *v = item == HUD_APP_SPORT_BAR ? &s->sport_bar : item == HUD_APP_ALWAYS ? &s->always : &s->nav;
    *v = !*v;
    }
}

int hud_app_wants_worker(const hud_settings *s) { return s->sport_bar || s->nav; }

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
    /* Car run 4: T2 (brightness cue: grey 3 through the revs, all white 4 at the shift, no backs) and T3 (nav, texts on the
       6WB areas 0x1A/0x1B). Owner: "this design was well for me"; run 5 confirmed it, frozen. */
    static const char *const k_fixed[] = {
        "--draw-live", "--layout", "big", "--flash-ms", "0", "--palette", "3,3,3,3,3,3,4", "--shift-colour", "4",
        "--backs", "none", "--most-inst", "2", "--handles", "31,32", "--canvas", "120x72", "--container", "23",
        "--text-handles", "26,27", "--text-visible", "--udp-port", "42873",
    };
    const char *paths[3];
    size_t used = 0, i;
    int argc = 0;
    char *p = buf;
    const char *fmt[3] = { "%s/hud-sportbar-qnx", "%s/shift-deck-qnx", "%s/settings.txt" };
    for (i = 0; i < 3; i++) {
        int n = snprintf(p, cap - used, fmt[i], dir);
        if (n < 0 || (size_t)n >= cap - used) return -1;
        paths[i] = p;
        p += n + 1;
        used += (size_t)n + 1;
    }
    if (max < (int)(sizeof k_fixed / sizeof k_fixed[0]) + 8) return -1;
    argv[argc++] = paths[0];
    for (i = 0; i < sizeof k_fixed / sizeof k_fixed[0]; i++) argv[argc++] = k_fixed[i];
    argv[argc++] = "--collector";
    argv[argc++] = paths[1];
    argv[argc++] = "--host";
    argv[argc++] = gateway;
    argv[argc++] = "--settings";
    argv[argc++] = paths[2];
    argv[argc] = NULL;
    return argc;
}

void hud_app_status(const hud_settings *s, int running, const char *conflict, int restarts, char *out, size_t cap)
{
    if (conflict) snprintf(out, cap, "Off: remove %s first (two HUD apps freeze the HUD)", conflict);
    else if (!hud_app_wants_worker(s)) snprintf(out, cap, "Off");
    else if (!running) snprintf(out, cap, restarts ? "Stopped, retrying (%d)" : "Starting", restarts);
    else if (s->sport_bar && !s->always) snprintf(out, cap, "Running. Driving mode not available yet: tick Always");
    else snprintf(out, cap, "Running");
}
