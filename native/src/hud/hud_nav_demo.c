#include "hud_nav_demo.h"

#include <stdio.h>
#include <string.h>

#define S  HUD_LANE_STRAIGHT
#define L  HUD_LANE_LEFT
#define R  HUD_LANE_RIGHT
#define SL HUD_LANE_SLIGHT_LEFT
#define SR HUD_LANE_SLIGHT_RIGHT
#define U  HUD_LANE_UTURN

typedef struct {
    enum hud_maneuver man;
    int exit, total_m;
    const char *street;
    int nlanes;
    hud_lane lane[HUD_MAX_LANES];   /* { arrows, kind, recommended, use (0: all its arrows) } */
} step;

static const step k_ua[] = {
    /* Kyiv avenue: left turn and U-turn share the leftmost lane. */
    { HUD_MAN_LEFT, 0, 700, "просп. Берестейський", 4,
      { { U | L, HUD_LANE_CAR, 1, L }, { S, HUD_LANE_CAR, 0, 0 }, { S, HUD_LANE_CAR, 0, 0 }, { S | R, HUD_LANE_CAR, 0, 0 } } },
    /* Straight on, with a bus lane on the right (marked А). */
    { HUD_MAN_STRAIGHT, 0, 500, "вул. Хрещатик", 4,
      { { S, HUD_LANE_CAR, 1, 0 }, { S, HUD_LANE_CAR, 1, 0 }, { S, HUD_LANE_CAR, 1, 0 }, { S | R, HUD_LANE_BUS, 0, 0 } } },
    /* The exit to the bridge on the right (the main road grey); the second-from-right lane can do both. */
    { HUD_MAN_EXIT_RIGHT, 0, 900, "Південний міст", 4,
      { { S, HUD_LANE_CAR, 0, 0 }, { S, HUD_LANE_CAR, 0, 0 }, { S | SR, HUD_LANE_CAR, 1, SR }, { SR, HUD_LANE_CAR, 1, 0 } } },
    /* A roundabout (кільце), third exit, so the left lanes. */
    { HUD_MAN_ROUNDABOUT, 3, 600, "вул. Велика Васильківська", 3,
      { { L | S, HUD_LANE_CAR, 1, 0 }, { S, HUD_LANE_CAR, 1, 0 }, { S | R, HUD_LANE_CAR, 0, 0 } } },
    /* Lviv: tram tracks in the left lane, turn right. */
    { HUD_MAN_RIGHT, 0, 400, "просп. Свободи", 3,
      { { S, HUD_LANE_TRAM, 0, 0 }, { S, HUD_LANE_CAR, 0, 0 }, { R, HUD_LANE_CAR, 1, 0 } } },
    /* Divided highway: a U-turn gap (розворот) from the left lane. */
    { HUD_MAN_UTURN, 0, 1500, "траса М-06", 3,
      { { U, HUD_LANE_CAR, 1, 0 }, { S, HUD_LANE_CAR, 0, 0 }, { S | SR, HUD_LANE_CAR, 0, 0 } } },
    /* Keep left at a fork (the other branch grey), five lanes; the second lane can go both ways. */
    { HUD_MAN_KEEP_LEFT, 0, 800, "Столичне шосе", 5,
      { { SL, HUD_LANE_CAR, 1, 0 }, { SL | S, HUD_LANE_CAR, 1, SL }, { S, HUD_LANE_CAR, 0, 0 }, { S, HUD_LANE_CAR, 0, 0 },
        { S | R, HUD_LANE_CAR, 0, 0 } } },
    { HUD_MAN_ARRIVE, 0, 300, "вул. Сагайдачного, 25", 0, { { 0, 0, 0, 0 } } },
};

/* What a phone may really send: other languages and scripts, long names, raw words a map would shorten,
   invisible and decomposed characters, emoji, and the most lanes with every marker (the batch budget). */
static const step k_intl[] = {
    /* German, the word ending -straße shortened; six lanes with both markers. */
    { HUD_MAN_LEFT, 0, 600, "Straße des 17. Juni", 6,
      { { U | L, HUD_LANE_CAR, 1, 0 }, { L | S, HUD_LANE_CAR, 1, 0 }, { S, HUD_LANE_CAR, 0, 0 }, { S, HUD_LANE_TRAM, 0, 0 },
        { S | R, HUD_LANE_CAR, 0, 0 }, { R, HUD_LANE_BUS, 0, 0 } } },
    /* Polish, written out in full: "ulica" becomes "ul.". */
    { HUD_MAN_ROUNDABOUT, 1, 500, "ulica Marszałkowska", 2,
      { { S, HUD_LANE_CAR, 0, 0 }, { S | R, HUD_LANE_CAR, 1, R } } },
    /* French, longer than the street line: cut on a consonant with a dot. */
    { HUD_MAN_RIGHT, 0, 700, "Avenue des Champs-Élysées", 3,
      { { S, HUD_LANE_CAR, 0, 0 }, { S, HUD_LANE_CAR, 0, 0 }, { R, HUD_LANE_CAR, 1, 0 } } },
    /* Raw Ukrainian from a phone: "вулиця", no-break space, «quotes», ї as і + U+0308, a left-to-right mark. */
    { HUD_MAN_UTURN, 0, 900, "вулиця\xC2\xA0\xC2\xAB\xD0\x9A\xD0\xB8\xD1\x96\xCC\x88\xD0\xB2\xD1\x81\xD1\x8C\xD0\xBA\xD0\xB0"
      "\xC2\xBB\xE2\x80\x8E  ", 4,
      { { U, HUD_LANE_CAR, 1, 0 }, { U | L, HUD_LANE_CAR, 1, 0 }, { S, HUD_LANE_CAR, 0, 0 }, { S, HUD_LANE_CAR, 0, 0 } } },
    /* An emoji the HUD can't show, and a fourth roundabout exit (all the way round). */
    { HUD_MAN_ROUNDABOUT, 4, 400, "\xF0\x9F\x9A\xA7 Обхід ремонту", 0, { { 0, 0, 0, 0 } } },
    /* Greek: shown as is in the native script, dropped in Latin. A sharp right. */
    { HUD_MAN_SHARP_RIGHT, 0, 800, "Λεωφόρος Συγγρού", 3,
      { { SL, HUD_LANE_CAR, 0, 0 }, { S, HUD_LANE_CAR, 0, 0 }, { S | SR, HUD_LANE_CAR, 1, 0 } } },
    /* English: "Street" becomes "St"; a far manoeuvre in km. */
    { HUD_MAN_SLIGHT_LEFT, 0, 12000, "Oxford Street", 2,
      { { SL, HUD_LANE_CAR, 1, 0 }, { S, HUD_LANE_CAR, 0, 0 } } },
    /* A long name with a house number, which survives the cut. */
    { HUD_MAN_ARRIVE, 0, 300, "Kärntner Straße Ringstraßen-Galerien 12", 0, { { 0, 0, 0, 0 } } },
};

static const step *k_route = k_ua;
static int k_count = (int)(sizeof k_ua / sizeof k_ua[0]);

int hud_nav_demo_use(const char *name)
{
    if (!strcmp(name, "ua")) { k_route = k_ua; k_count = (int)(sizeof k_ua / sizeof k_ua[0]); return 0; }
    if (!strcmp(name, "intl")) { k_route = k_intl; k_count = (int)(sizeof k_intl / sizeof k_intl[0]); return 0; }
    return -1;
}

enum { RUN_MS = 8000, HOLD_MS = 1000, STEP_MS = RUN_MS + HOLD_MS };

int hud_nav_demo_count(void) { return k_count; }
uint32_t hud_nav_demo_length_ms(void) { return (uint32_t)hud_nav_demo_count() * STEP_MS; }

void hud_nav_demo_get(int i, int dist_m, hud_nav_info *out)
{
    const step *s = &k_route[i % hud_nav_demo_count()];
    memset(out, 0, sizeof *out);
    out->man = s->man;
    out->exit = s->exit;
    out->total_m = s->total_m;
    out->dist_m = dist_m;
    snprintf(out->street, sizeof out->street, "%s", s->street);
    out->nlanes = s->nlanes;
    memcpy(out->lane, s->lane, sizeof out->lane);
}

void hud_nav_demo_at(uint32_t t_ms, hud_nav_info *out)
{
    uint32_t t = t_ms % hud_nav_demo_length_ms();
    int i = (int)(t / STEP_MS);
    uint32_t in = t % STEP_MS;
    int total = k_route[i].total_m;
    int d = in >= RUN_MS ? 0 : (int)(total - (double)total * in / RUN_MS);
    hud_nav_demo_get(i, d, out);
}
