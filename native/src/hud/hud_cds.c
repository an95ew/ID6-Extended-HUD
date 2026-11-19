#include "hud_cds.h"

#include "drive_frame.h"

#include <stdlib.h>
#include <string.h>

static const char *const k_kinds[] = { "log", "rpm", "kph", "gear", "mode" };

int hud_cds_parse(const char *text, size_t len, hud_cds_prop *out, int max)
{
    size_t i = 0;
    int n = 0;
    while (i < len && n < max) {
        char line[160], name[HUD_CDS_NAME], kind[16];
        size_t k = 0;
        unsigned interval = 100, j;
        int got;
        while (i < len && text[i] != '\n') {
            if (k + 1 < sizeof line) line[k++] = text[i];
            i++;
        }
        i++;
        line[k] = 0;
        if (k && line[k - 1] == '\r') line[k - 1] = 0;
        if (line[0] == '#') continue;
        kind[0] = 0;
        got = 0;
        {
            char *p = line, *e;
            while (*p == ' ' || *p == '\t') p++;
            for (e = p; *e && *e != ' ' && *e != '\t'; e++) {}
            if (e == p || (size_t)(e - p) >= sizeof name) continue;
            memcpy(name, p, (size_t)(e - p));
            name[e - p] = 0;
            got = 1;
            for (p = e; *p == ' ' || *p == '\t'; p++) {}
            for (e = p; *e && *e != ' ' && *e != '\t'; e++) {}
            if (e > p && (size_t)(e - p) < sizeof kind) { memcpy(kind, p, (size_t)(e - p)); kind[e - p] = 0; }
            for (p = e; *p == ' ' || *p == '\t'; p++) {}
            if (*p) {
                char *end;
                unsigned long v = strtoul(p, &end, 10);
                if (end == p || v > 60000) continue;
                interval = (unsigned)v;
            }
        }
        if (!got) continue;
        out[n].kind = HUD_CDS_LOG;
        if (kind[0]) {
            for (j = 0; j < sizeof k_kinds / sizeof k_kinds[0] && strcmp(kind, k_kinds[j]); j++) {}
            if (j == sizeof k_kinds / sizeof k_kinds[0]) continue;
            out[n].kind = (hud_cds_kind)j;
        }
        strcpy(out[n].name, name);
        out[n].interval = interval;
        n++;
    }
    return n;
}

int hud_cds_value(const uint16_t *s, int max, char *ascii, size_t cap, long *number)
{
    int n;
    size_t k = 0;
    *number = -1;
    if (!s) { if (cap) ascii[0] = 0; return 0; }
    for (n = 0; n < max && s[n]; n++) {
        uint16_t c = s[n];
        if (k + 1 < cap) ascii[k++] = c >= 0x20 && c < 0x7f ? (char)c : '?';
        if (*number < 0 && c >= '0' && c <= '9') {
            int neg = n > 0 && s[n - 1] == '-';
            long v = 0;
            int m = n;
            while (m < max && s[m] >= '0' && s[m] <= '9' && v < 100000000L) v = v * 10 + (s[m++] - '0');
            *number = neg ? -v : v;
            if (*number < 0) *number = -1;   /* negatives are not RPM, speed, gear or mode */
        }
    }
    if (cap) ascii[k] = 0;
    return n;
}

int hud_cds_frame(hud_cds_kind kind, long number, uint64_t now_ms, struct drive_frame *f)
{
    if (kind == HUD_CDS_LOG || number < 0 || number > 0x7fffffffL) return 0;
    f->sampled_ms = now_ms;
    f->rpm = kind == HUD_CDS_RPM ? (int32_t)number : -1;
    f->kph = kind == HUD_CDS_KPH ? (int32_t)number : -1;
    f->gear_raw = kind == HUD_CDS_GEAR ? (int32_t)number : -1;
    f->mode_raw = kind == HUD_CDS_MODE ? (int32_t)number : -1;
    f->source = DRIVE_SRC_CDS;
    f->reserved = 0;
    return 1;
}
