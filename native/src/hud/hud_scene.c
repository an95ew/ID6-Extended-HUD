#include "hud_scene.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void hud_settings_default(hud_settings *s)
{
    s->sport_bar = 1;
    s->always = 0;
    s->nav = 1;
    s->shift_rpm = 7000;
    hud_text_style_default(&s->text);
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
        { "nav", offsetof(hud_settings, nav), 0, 1 },
        { "shift_rpm", offsetof(hud_settings, shift_rpm), 4000, 7500 },
        { "street_max", offsetof(hud_settings, text.street_max), 8, HUD_TEXT_UNITS },
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
            int lang;
            for (k = 0; k < sizeof keys / sizeof keys[0]; k++)
                if (strlen(keys[k].key) == kn && !memcmp(text + start, keys[k].key, kn) &&
                    take_int(v, vn, keys[k].lo, keys[k].hi, (int *)((char *)s + keys[k].off)))
                    taken++;
            if (kn == 4 && !memcmp(text + start, "lang", 4) && (lang = hud_lang_parse(v, vn)) >= 0) {
                s->text.lang = lang;
                taken++;
            }
            if (kn == 4 && !memcmp(text + start, "text", 4)) {
                if (vn == 6 && !memcmp(v, "native", 6)) { s->text.script = HUD_SCRIPT_NATIVE; taken++; }
                if (vn == 5 && !memcmp(v, "latin", 5)) { s->text.script = HUD_SCRIPT_LATIN; taken++; }
            }
        }
        i++;
    }
    return taken;
}

int hud_settings_format(const hud_settings *s, char *out, size_t cap)
{
    return snprintf(out, cap, "sport_bar=%d\nalways=%d\nnav=%d\nshift_rpm=%d\n"
                    "lang=%s\ntext=%s\nstreet_max=%d\n",
                    s->sport_bar, s->always, s->nav, s->shift_rpm, hud_lang_name(s->text.lang),
                    s->text.script == HUD_SCRIPT_LATIN ? "latin" : "native", s->text.street_max);
}

hud_scene hud_scene_select(const hud_settings *s, const hud_state *st)
{
    int sport = st->mode == HUD_MODE_SPORT || st->mode == HUD_MODE_SPORT_PLUS;
    if (st->factory_nav)
        return HUD_SCENE_NONE;
    if (s->sport_bar && (s->always || sport))
        return HUD_SCENE_RPM;
    return s->nav && st->route_active ? HUD_SCENE_NAV : HUD_SCENE_NONE;
}

const char *hud_nav_paused_reason(const hud_settings *s)
{
    return s->nav && s->sport_bar && s->always ? "Navigation paused: Sport Bar set to Always" : NULL;
}
