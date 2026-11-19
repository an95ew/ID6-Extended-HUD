#include "hud/hud_nav.h"
#include "hud/hud_nav_demo.h"
#include "hud/hud_proto.h"
#include "hud/hud_scene.h"
#include "hud/hud_sender.h"
#include "hud/hud_app.h"
#include "hud/hud_gear.h"
#include "hud/hud_cds.h"
#include "drive_frame.h"
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include "hud/hud_strip.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const hud_call *last(const hud_batch *b) { return &b->call[b->n - 1]; }

static void test_proto(void) {
    hud_batch b;
    hud_point pts[HUD_MAX_POINTS + 1];
    char line[2048];
    unsigned i;
    memset(pts, 0, sizeof pts);
    hud_batch_init(&b);
    /* The fill colour call seen in the packaged worker's draw(): 00 05 04. */
    assert(hud_fill_color(&b, 5, 4) == 0);
    assert(last(&b)->fkt == 0x21B && last(&b)->len == 3 && !memcmp(last(&b)->p, "\x00\x05\x04", 3));
    assert(hud_set_visibility(&b, 1, 1) == 0 && last(&b)->len == 3 && last(&b)->p[2] == 1);
    assert(hud_switch_visibility(&b, 5, 6) == 0 && !memcmp(last(&b)->p, "\x00\x05\x00\x06", 4));
    assert(hud_remove_all(&b, 6) == 0 && last(&b)->len == 2);
    assert(hud_get_size(&b, 5) == 0 && last(&b)->fkt == 0xC04);
    /* Polygon: handle, id, kind 3, count, then x/y big-endian. */
    pts[0].x = 258; pts[0].y = -2;
    assert(hud_polygon(&b, 5, 0x0102, pts, 1) == 0 && last(&b)->len == 10);   /* the shortest the worker accepts */
    hud_batch_init(&b);
    assert(hud_polygon(&b, 5, 0x0102, pts, 4) == 0);
    assert(last(&b)->len == 22 && last(&b)->p[2] == 1 && last(&b)->p[3] == 2 && last(&b)->p[4] == 3 && last(&b)->p[5] == 4);
    assert(last(&b)->p[6] == 0x01 && last(&b)->p[7] == 0x02 && last(&b)->p[8] == 0xFF && last(&b)->p[9] == 0xFE);
    assert(hud_polygon(&b, 5, 1, pts, HUD_MAX_POINTS) == 0 && last(&b)->len == HUD_MAX_PAYLOAD);
    assert(hud_polygon(&b, 5, 1, pts, HUD_MAX_POINTS + 1) == -1);
    assert(hud_polygon(&b, 5, 1, pts, 0) == -1);
    /* Cyrillic street names: UTF-8 in, UTF-16BE out. */
    assert(hud_text_utf8(&b, 2, "вул.") == 0 && last(&b)->len == 13 &&
           !memcmp(last(&b)->p, "\x00\x02\x00\x04\x32\x04\x43\x04\x3B\x00\x2E\x00\x00", 13));
    assert(hud_text_utf8(&b, 2, "a\xF0\x9F\x9A\x97" "b") == 0 && last(&b)->p[6] == '?' && last(&b)->len == 11);
    /* Text: handle, 0, UTF-16BE, 0000; 32 characters at most. */
    assert(hud_text(&b, 3, "6800") == 0 && last(&b)->len == 13 && !memcmp(last(&b)->p, "\x00\x03\x00\x00\x36\x00\x38", 7));
    assert(hud_text(&b, 3, "") == 0 && last(&b)->len == 5);
    assert(hud_text(&b, 3, "123456789012345678901234567890123") == -1);
    /* Handle 0 is refused, and the batch is left as it was. */
    i = b.n;
    assert(hud_fill_color(&b, 0, 4) == -1 && b.n == i);
    hud_batch_init(&b);
    for (i = 0; i < HUD_MAX_CALLS; i++) assert(hud_remove_all(&b, 5) == 0);
    assert(hud_remove_all(&b, 5) == -1 && b.n == HUD_MAX_CALLS);
    /* The validator refuses what the packaged worker refuses. */
    {
        hud_call c;
        memset(&c, 0, sizeof c);
        c.fkt = HUD_SET_VISIBILITY; c.len = 3; c.p[1] = 1; c.p[2] = 2;
        assert(!hud_call_valid(&c));
        c.fkt = 0x215; c.p[2] = 0;   /* ImageSetRotation is never sent */
        assert(!hud_call_valid(&c));
        c.fkt = HUD_CIRCLE; c.len = 19; c.p[4] = 3;
        c.p[11] = 0x00; c.p[12] = 0x00; c.p[13] = 0x1C; c.p[14] = 0x21;   /* 7201 */
        assert(!hud_call_valid(&c));
        c.p[14] = 0x20;                                                  /* 7200 */
        assert(hud_call_valid(&c));
    }
    hud_batch_init(&b);
    hud_fill_color(&b, 5, 4);
    assert(hud_call_format(&b.call[0], line, sizeof line) > 0 && !strcmp(line, "21B ImageSetFillColor 5 3 000504"));
}

static void test_fields(void) {
    hud_profile p;
    double lo, hi;
    int i;
    hud_profile_b58(&p);
    /* Owner, 2026-11-18: low 1000-5000, middle 5000-6000, tallest 6000 to the 7000 redline; the zones follow the end. */
    assert(p.shift == 7000 && p.yellow == 5000 && p.red == 6000);
    hud_field_span(&p, 0, &lo, &hi);
    assert(lo == 1000 && hi > 1666 && hi < 1667);
    hud_field_span(&p, 5, &lo, &hi);
    assert(hi == 5000);
    hud_field_span(&p, 6, &lo, &hi);
    assert(lo == 5000);
    hud_field_span(&p, 11, &lo, &hi);
    assert(hi == 7000 && lo > 6666 && lo < 6667);
    hud_profile_for_shift(&p, 6800);
    assert(p.yellow == 4800 && p.red == 5800);
    for (i = 0; i < HUD_FIELDS; i++) assert(hud_field_zone(i) == (i < 6 ? 0 : i < 9 ? 1 : 2));
}

static void steady(hud_anim *a, double rpm, uint32_t t) {
    uint32_t k;
    hud_anim_reset(a);
    for (k = 0; k <= 2000; k += 10) hud_anim_step(a, rpm, t - 2000 + k);
}

static unsigned count(const hud_batch *b, uint16_t fkt) {
    unsigned i, n = 0;
    for (i = 0; i < b->n; i++) n += b->call[i].fkt == fkt;
    return n;
}

static void test_anim(void) {
    hud_anim a;
    hud_anim_reset(&a);
    hud_anim_step(&a, 3000, 0);
    assert(a.rpm == 3000);
    hud_anim_step(&a, 6000, 45);
    assert(a.rpm > 4800 && a.rpm < 4950);          /* 1 - 1/e of the way */
    hud_anim_step(&a, 6000, 300);
    assert(a.rpm > 5980);                          /* one step is capped at 200 ms */
    /* Peak hold: stays 400 ms after a drop, then falls at 6000 rpm/s. */
    hud_anim_step(&a, 2000, 310);
    hud_anim_step(&a, 2000, 700);
    assert(a.peak > 5980);
    hud_anim_step(&a, 2000, 800);
    assert(a.peak < 5450 && a.peak > 5340);
    /* Flash: 125 ms on, 125 ms off. */
    a.now_ms = 0; assert(hud_anim_flash_on(&a));
    a.now_ms = 124; assert(hud_anim_flash_on(&a));
    a.now_ms = 125; assert(!hud_anim_flash_on(&a));
    a.now_ms = 250; assert(hud_anim_flash_on(&a));
}

static void check_polygons(const hud_batch *b, const hud_geom *g);

static void test_frames(void) {
    hud_profile p; hud_palette pal; hud_geom g; hud_handles h;
    hud_anim a;
    hud_batch b;
    int shown = 0, worst = 0, n, rpm, gear;
    uint32_t t;
    hud_profile_b58(&p); hud_palette_default(&pal); hud_geom_default(&g); hud_handles_default(&h);

    /* The default palette draws every unlit field grey: one colour change for the twelve. */
    steady(&a, 800, 10000);
    hud_batch_init(&b);
    assert(hud_strip_frame(&b, &p, &pal, &g, &h, &a, &shown) == 1 + 1 + 12 + 1);
    shown = 0;

    /* The rest uses seven different indices, so every colour change shows. */
    pal.base_off = 1; pal.base_on = 4; pal.yellow_off = 2; pal.yellow_on = 5; pal.red_off = 3; pal.red_on = 6; pal.ink = 7;
    /* Idle: twelve unlit fields in three colours, drawn on the hidden image 6, then switched in. */
    hud_batch_init(&b);
    n = hud_strip_frame(&b, &p, &pal, &g, &h, &a, &shown);
    assert(n == 1 + 3 + 12 + 1 && shown == 1);
    assert(b.call[0].fkt == HUD_REMOVE_ALL && hud_call_handle(&b.call[0]) == 6);
    assert(last(&b)->fkt == HUD_SWITCH_VISIBILITY && !memcmp(last(&b)->p, "\x00\x05\x00\x06", 4));
    /* The next frame draws on 5. */
    hud_batch_init(&b);
    hud_strip_frame(&b, &p, &pal, &g, &h, &a, &shown);
    assert(hud_call_handle(&b.call[0]) == 5 && shown == 0);

    /* 6,200 rpm: 9 fields full (6 base, 3 yellow), the first red one partly, 3 unlit. */
    steady(&a, 6200, 10000);
    hud_batch_init(&b);
    hud_strip_frame(&b, &p, &pal, &g, &h, &a, &shown);
    assert(count(&b, HUD_POLYGON) == 3 + 9 + 1);
    /* 7,000 rpm, the bar's end, is the shift point. */
    steady(&a, 7000, 10000);
    assert(hud_anim_shifting(&p, &a));
    steady(&a, 6980, 10000);
    assert(!hud_anim_shifting(&p, &a));

    /* Shift point, flash on: every field lit, no unlit backs; off: every field unlit. */
    steady(&a, 7100, 10000);
    assert(hud_anim_shifting(&p, &a) && hud_anim_flash_on(&a));
    hud_batch_init(&b);
    hud_strip_frame(&b, &p, &pal, &g, &h, &a, &shown);
    assert(count(&b, HUD_POLYGON) == 12 && count(&b, HUD_FILL_COLOR) == 3);
    steady(&a, 7100, 10125);
    hud_batch_init(&b);
    hud_strip_frame(&b, &p, &pal, &g, &h, &a, &shown);
    assert(count(&b, HUD_POLYGON) == 12);

    /* Every frame fits the 48-call batch, at any RPM, gear and flash phase, with a peak marker. */
    for (gear = 0; gear <= 8; gear++)
        for (rpm = 0; rpm <= 7600; rpm += 25)
            for (t = 10000; t < 10250; t += 125) {
                steady(&a, rpm, t);
                a.gear = gear;
                a.peak = rpm + 300;
                hud_batch_init(&b);
                n = hud_strip_frame(&b, &p, &pal, &g, &h, &a, &shown);
                assert(n > 0 && n <= HUD_MAX_CALLS);
                if (n > worst) worst = n;
            }
    printf("worst frame: %d calls\n", worst);

    /* With the unlit fields on a static image, a frame needs fewer calls. No such image is known yet: 2-4
       are text areas. */
    h.static_backs = 1;
    h.static_h = 7;
    steady(&a, 3000, 10000);
    hud_batch_init(&b);
    n = hud_strip_frame(&b, &p, &pal, &g, &h, &a, &shown);
    assert(count(&b, HUD_POLYGON) == 2 + 1 && n == 1 + 1 + 3 + 1);
    hud_batch_init(&b);
    assert(hud_strip_static(&b, &p, &pal, &g, &h) == 1 + 3 + 12 && hud_call_handle(&b.call[0]) == 7);

    /* Equal keys mean equal frames, so the worker can skip them. */
    {
        hud_strip_key k1, k2;
        steady(&a, 3000, 10000);
        k1 = hud_strip_key_of(&p, &g, &a);
        k2 = hud_strip_key_of(&p, &g, &a);
        assert(hud_strip_key_equal(&k1, &k2));
        steady(&a, 3200, 10000);
        k2 = hud_strip_key_of(&p, &g, &a);
        assert(!hud_strip_key_equal(&k1, &k2));
    }

    /* Swatches: 16 colours, 1 + 16 x 2 calls. */
    hud_batch_init(&b);
    assert(hud_swatches(&b, 5, &g, 0) == 33 && b.call[1].p[2] == 0 && b.call[31].p[2] == 15);

    /* The layout scales with the canvas. */
    hud_geom_for_canvas(&g, 960, 360);
    assert(g.x == 516 && g.w == 416 && g.gear_h == 88);

    /* Run 2: on the HUD's 120x72 image the scaled layout used x 64..116, y 22..34. The big one fills it, peak marker
       and gear digit included, and every frame still fits a batch. */
    hud_geom_big(&g, 120, 72);
    hud_handles_default(&h);   /* backs drawn every frame, as on the car */
    assert(g.x + g.w <= 120 && g.w >= 110 && g.y - 8 >= 0 && g.gear_top > g.y + g.h);
    assert(g.gear_top + g.gear_h <= 72 && g.gear_h >= 28);
    for (gear = 0; gear <= 8; gear++)
        for (rpm = 0; rpm <= 7600; rpm += 50) {
            steady(&a, rpm, 10000);
            a.gear = gear;
            a.peak = rpm + 300;
            hud_batch_init(&b);
            assert(hud_strip_frame(&b, &p, &pal, &g, &h, &a, &shown) > 0);
            check_polygons(&b, &g);
        }
    /* Run 3: yellow and red came out the same orange on the HUD (no red index), so the zones differ in height:
       at idle every field is drawn, base lowest, red tallest. */
    steady(&a, 800, 10000);
    a.gear = 0;
    hud_batch_init(&b);
    hud_strip_frame(&b, &p, &pal, &g, &h, &a, &shown);
    {
        int tops[3] = { 999, 999, 999 }, zone = 0, seen = 0;
        for (n = 0; n < (int)b.n; n++)
            if (b.call[n].fkt == HUD_POLYGON) {
                int y = b.call[n].p[8] << 8 | b.call[n].p[9];
                zone = seen < 6 ? 0 : seen < 9 ? 1 : 2;   /* backs are drawn zone by zone */
                if (y < tops[zone]) tops[zone] = y;
                seen++;
            }
        assert(seen == 12 && tops[0] > tops[1] && tops[1] > tops[2]);
    }

    /* At ~1.5 s per frame (run 2) a 125 ms flash is sampled at random: flash_ms 0 holds the shift state lit. */
    p.flash_ms = 0;
    for (t = 10000; t < 12000; t += 125) {
        steady(&a, 7100, t);
        assert(hud_strip_key_of(&p, &g, &a).flash_on);
    }
    p.flash_ms = 1500;
    steady(&a, 7100, 3000); assert(hud_strip_key_of(&p, &g, &a).flash_on);
    steady(&a, 7100, 1500); assert(!hud_strip_key_of(&p, &g, &a).flash_on);
    hud_profile_b58(&p);
    assert(p.flash_ms == 125);

    /* Run 4 shift cue: lit fields dim (3) through the revs, the whole bar white (4) from the shift point, no backs. */
    hud_palette_default(&pal);
    pal.base_on = pal.yellow_on = pal.red_on = 3;
    pal.shift_on = 4;
    pal.backs = HUD_BACKS_NONE;
    p.flash_ms = 0;
    steady(&a, 800, 10000); a.gear = 0;
    hud_batch_init(&b);
    assert(hud_strip_frame(&b, &p, &pal, &g, &h, &a, &shown) == 2);          /* idle: RemoveAll + switch, nothing lit */
    steady(&a, 6200, 10000); a.gear = 3;
    hud_batch_init(&b);
    hud_strip_frame(&b, &p, &pal, &g, &h, &a, &shown);
    assert(count(&b, HUD_FILL_COLOR) == 2 && count(&b, HUD_POLYGON) == 9 + 1 + 5);   /* 3 for fields, 4 for the "3" */
    steady(&a, 7100, 10000); a.gear = 3;   /* steady() resets the gear */
    hud_batch_init(&b);
    hud_strip_frame(&b, &p, &pal, &g, &h, &a, &shown);
    for (n = 0; n < (int)b.n; n++)
        if (b.call[n].fkt == HUD_FILL_COLOR) assert(b.call[n].p[2] == 4);   /* bar and digit white */
    assert(count(&b, HUD_POLYGON) == 12 + 5);
    /* Outline backs: line width 1, line colour base_off, fill 0, then the unlit fields. */
    pal.backs = HUD_BACKS_OUTLINE;
    steady(&a, 800, 10000); a.gear = 0;
    hud_batch_init(&b);
    hud_strip_frame(&b, &p, &pal, &g, &h, &a, &shown);
    assert(b.call[1].fkt == HUD_LINE_WIDTH && b.call[2].fkt == HUD_LINE_COLOR && b.call[2].p[2] == pal.base_off);
    assert(b.call[3].fkt == HUD_FILL_COLOR && b.call[3].p[2] == 0 && count(&b, HUD_POLYGON) == 12);
    for (gear = 0; gear <= 8; gear++)
        for (rpm = 0; rpm <= 7600; rpm += 100) {
            steady(&a, rpm, 10000); a.gear = gear; a.peak = rpm + 300;
            hud_batch_init(&b);
            assert(hud_strip_frame(&b, &p, &pal, &g, &h, &a, &shown) > 0);
        }
    hud_palette_default(&pal);
    hud_profile_b58(&p);

    /* Big swatch bars over the whole 120x72 image: n colours, then a white foot under each. */
    hud_batch_init(&b);
    assert(hud_swatch_bars(&b, 32, &g, 40, 8, 0, 4) == 1 + 8 * 2 + 1 + 8);
    assert(b.call[1].fkt == HUD_FILL_COLOR && b.call[1].p[2] == 40 && b.call[15].p[2] == 47);
    for (n = 0; n < (int)b.n; n++)
        if (b.call[n].fkt == HUD_POLYGON) {
            int k;
            for (k = 0; k < 4; k++) {
                int x = b.call[n].p[6 + 4 * k] << 8 | b.call[n].p[7 + 4 * k];
                int y = b.call[n].p[8 + 4 * k] << 8 | b.call[n].p[9 + 4 * k];
                assert(x >= 0 && x <= 120 && y >= 0 && y <= 72);
            }
        }
    /* Line colour: fill 0 and width 3 first, the index as the line colour, the feet in white for both. */
    hud_batch_init(&b);
    assert(hud_swatch_bars(&b, 32, &g, 0, 4, 1, 4) == 1 + 2 + 4 * 2 + 2 + 4);
    assert(b.call[1].fkt == HUD_FILL_COLOR && b.call[1].p[2] == 0 && b.call[2].fkt == HUD_LINE_WIDTH);
    assert(b.call[3].fkt == HUD_LINE_COLOR && b.call[3].p[2] == 0 && b.call[9].fkt == HUD_LINE_COLOR && b.call[9].p[2] == 3);
    hud_batch_init(&b);
    assert(hud_swatch_bars(&b, 32, &g, 0, 9, 0, 4) == -1 && b.n == 0);
}

static void test_scene(void) {
    hud_settings s;
    hud_state st;
    char text[256];
    hud_settings_default(&s);
    assert(s.shift_rpm == 7000);
    memset(&st, 0, sizeof st);
#define SCENE(sb, al, nv, mode_, route) (s.sport_bar = sb, s.always = al, s.nav = nv, st.mode = mode_, st.route_active = route, hud_scene_select(&s, &st))
    /* The table in menu-spec.md. */
    assert(SCENE(1, 0, 1, HUD_MODE_SPORT, 1) == HUD_SCENE_RPM);
    assert(SCENE(1, 0, 1, HUD_MODE_SPORT_PLUS, 0) == HUD_SCENE_RPM);
    assert(SCENE(1, 0, 1, HUD_MODE_COMFORT, 1) == HUD_SCENE_NAV);
    assert(SCENE(1, 0, 1, HUD_MODE_ECO_PRO, 0) == HUD_SCENE_NONE);
    assert(SCENE(1, 0, 0, HUD_MODE_SPORT, 1) == HUD_SCENE_RPM);
    assert(SCENE(1, 0, 0, HUD_MODE_COMFORT, 1) == HUD_SCENE_NONE);
    assert(SCENE(1, 1, 1, HUD_MODE_COMFORT, 1) == HUD_SCENE_RPM);
    assert(SCENE(1, 1, 0, HUD_MODE_ECO_PRO, 0) == HUD_SCENE_RPM);
    assert(SCENE(0, 1, 1, HUD_MODE_SPORT, 1) == HUD_SCENE_NAV);
    assert(SCENE(0, 0, 1, HUD_MODE_COMFORT, 1) == HUD_SCENE_NAV);
    assert(SCENE(0, 0, 0, HUD_MODE_SPORT, 1) == HUD_SCENE_NONE);
    /* An unreadable driving mode behaves as Comfort. */
    assert(SCENE(1, 0, 1, HUD_MODE_UNKNOWN, 1) == HUD_SCENE_NAV);
    assert(SCENE(1, 0, 0, HUD_MODE_UNKNOWN, 1) == HUD_SCENE_NONE);
    /* The factory navigation always wins. */
    st.factory_nav = 1;
    assert(SCENE(1, 1, 1, HUD_MODE_SPORT, 1) == HUD_SCENE_NONE);
    st.factory_nav = 0;
    /* Sport keeps the bar whatever the route does: navigation never interrupts it. */
    assert(SCENE(1, 0, 1, HUD_MODE_SPORT_PLUS, 1) == HUD_SCENE_RPM);
#undef SCENE
    /* "Always" pauses Navigation without unticking it. */
    s.sport_bar = 1; s.always = 1; s.nav = 1;
    assert(hud_nav_paused_reason(&s) && s.nav == 1);
    s.always = 0;
    assert(!hud_nav_paused_reason(&s));

    /* Settings file. */
    hud_settings_default(&s);
    assert(hud_settings_parse(&s, "always=1\r\nnav=0\nshift_rpm=6500\nbogus=3\nsport_bar=7\n", 49) == 3);
    assert(s.always == 1 && s.nav == 0 && s.shift_rpm == 6500 && s.sport_bar == 1);
    assert(hud_settings_parse(&s, "shift_rpm=99999\n", 16) == 0 && s.shift_rpm == 6500);
    assert(hud_settings_parse(&s, "lang=en\ntext=latin\nstreet_max=24\ntext=nativ\nlang=xx\nstreet_max=99\nturns_in_sport=1\n", 83) == 3);
    assert(s.text.lang == HUD_LANG_EN && s.text.script == HUD_SCRIPT_LATIN && s.text.street_max == 24);
    /* the latency knobs (car run 6 tunes them by hand): a window of 1..16, merging -1 (the layout's) .. 99 */
    assert(s.in_flight == 32 && s.merge_pts == -1 && s.blink_rpm == 6400);
    assert(hud_settings_parse(&s, "blink_rpm=6600\n", 15) == 1 && s.blink_rpm == 6600);
    assert(hud_settings_parse(&s, "in_flight=8\nmerge_pts=0\nin_flight=33\nmerge_pts=100\n", 51) == 2);
    assert(s.in_flight == 8 && s.merge_pts == 0);
    hud_settings_format(&s, text, sizeof text);
    {
        hud_settings r;
        memset(&r, 0, sizeof r);
        assert(hud_settings_parse(&r, text, strlen(text)) == 10 && !memcmp(&r, &s, sizeof r));
    }
}

static hud_call g_sent[256];
static unsigned g_nsent;
static int g_fail;
static int rec_send(void *u, const hud_call *c) {
    (void)u;
    if (g_fail) return -1;
    g_sent[g_nsent++ % 256] = *c;
    return 0;
}

static void answer(hud_sender *s, unsigned i, uint32_t t) {
    hud_sender_answer(s, g_sent[i].fkt, 0xC, g_sent[i].p, 2, t);
}

static void test_sender(void) {
    hud_sender s;
    hud_batch b, b2;
    unsigned i;
    hud_batch_init(&b);
    for (i = 0; i < 10; i++) hud_fill_color(&b, 5, (uint8_t)i);
    g_nsent = 0;
    hud_sender_init(&s, rec_send, NULL);
    hud_sender_submit(&s, &b, 0);
    assert(g_nsent == HUD_IN_FLIGHT);              /* the window */
    {   /* a wider window (car run 6: --in-flight) sends more before the first answer; it is clamped */
        hud_sender w;
        unsigned before = g_nsent;
        hud_sender_init(&w, rec_send, NULL);
        hud_sender_set_window(&w, 8);
        hud_sender_submit(&w, &b, 0);
        assert(g_nsent - before == 8);
        hud_sender_set_window(&w, 99);
        assert(w.window == HUD_MAX_IN_FLIGHT);
        hud_sender_set_window(&w, 0);
        assert(w.window == 1);
        g_nsent = before;
    }
    answer(&s, 0, 5);
    assert(g_nsent == 4);
    hud_sender_answer(&s, HUD_FILL_COLOR, 0x6, g_sent[1].p, 2, 6);   /* an ack is not the answer */
    assert(g_nsent == 4);
    hud_sender_answer(&s, HUD_POLYGON, 0xC, g_sent[1].p, 2, 6);      /* no such call in flight */
    assert(s.st.strays == 1);
    /* A newer batch waits; a third replaces the second. */
    hud_batch_init(&b2);
    hud_remove_all(&b2, 6);
    hud_sender_submit(&s, &b2, 7);
    hud_sender_submit(&s, &b2, 8);
    assert(s.st.replaced == 1);
    for (i = 1; i < 10; i++) answer(&s, i, 20);
    assert(s.st.done == 1 && s.st.last_batch_ms == 20 && s.st.answers == 10);
    assert(g_nsent == 11 && g_sent[10].fkt == HUD_REMOVE_ALL);
    answer(&s, 10, 25);
    assert(hud_sender_idle(&s) && s.st.done == 2);
    /* Errors count as answers; a silent terminal times the frame out. */
    hud_sender_submit(&s, &b, 100);
    hud_sender_answer(&s, g_sent[11].fkt, 0xF, g_sent[11].p, 2, 101);
    assert(s.st.errors == 1);
    hud_sender_tick(&s, 100 + HUD_ANSWER_TIMEOUT_MS);
    assert(!hud_sender_idle(&s));
    hud_sender_tick(&s, 101 + HUD_ANSWER_TIMEOUT_MS);
    assert(s.st.timeouts == 1 && s.st.failed == 1 && hud_sender_idle(&s));
    /* Car run 4 T0-1.log: one call never answered and no next frame queued, then "392 timeouts" on page 2. The dropped
       frame's calls must not stay in flight and be counted again on every pump. */
    {
        uint32_t t;
        for (t = 120 + HUD_ANSWER_TIMEOUT_MS; t < 10000; t += 20) hud_sender_tick(&s, t);
        assert(s.st.timeouts == 1 && s.st.failed == 1 && s.nflight == 0);
    }
    /* A transport refusal fails the frame. */
    g_fail = 1;
    hud_sender_submit(&s, &b, 3000);
    assert(s.st.failed == 2 && hud_sender_idle(&s));
    g_fail = 0;
    /* OpType 6 is answered with ResultAck (op D) on the car, e.g. ImageGetSize 49 -> 00 31 01 9c 01 49
       (car run 1, A3b). It is the answer, and the whole batch goes out. */
    {
        static const uint8_t size49[6] = { 0x00, 0x31, 0x01, 0x9c, 0x01, 0x49 };
        hud_sender_init(&s, rec_send, NULL);
        g_nsent = 0;
        hud_batch_init(&b2);
        for (i = 49; i < 49 + 5; i++) hud_get_size(&b2, (uint16_t)i);
        hud_sender_submit(&s, &b2, 0);
        hud_sender_answer(&s, HUD_GET_SIZE, 0xD, size49, 6, 3);
        assert(s.st.answers == 1 && s.st.errors == 0 && g_nsent == 4);
        for (i = 1; i < 5; i++) hud_sender_answer(&s, HUD_GET_SIZE, 0xD, g_sent[i].p, 2, 4);
        assert(s.st.done == 1 && s.st.timeouts == 0 && hud_sender_idle(&s));
        /* ProcessingAck (op A) is only a note; ErrorAck (op 9) is an error answer. */
        hud_sender_submit(&s, &b2, 10);
        hud_sender_answer(&s, HUD_GET_SIZE, 0xA, g_sent[5].p, 2, 11);
        assert(s.st.answers == 5);
        hud_sender_answer(&s, HUD_GET_SIZE, 0x9, g_sent[5].p, 2, 12);
        assert(s.st.answers == 6 && s.st.errors == 1);
    }
    /* Car run 6: pipelining. A frame all sent lets the next one start (into the other buffer) before its answers come;
       never more than two overlap, and a timeout drops both. */
    {
        hud_batch a1, a2;
        hud_batch_init(&a1);
        hud_batch_init(&a2);
        for (i = 0; i < 4; i++) { hud_fill_color(&a1, 31, (uint8_t)i); hud_fill_color(&a2, 32, (uint8_t)i); }
        hud_sender_init(&s, rec_send, NULL);
        hud_sender_set_window(&s, 32);
        g_nsent = 0;
        hud_sender_submit(&s, &a1, 0);
        assert(g_nsent == 4 && !hud_sender_ready(&s));    /* not pipelining: wait for the answers */
        hud_sender_set_pipeline(&s, 1);
        assert(hud_sender_ready(&s));
        hud_sender_submit(&s, &a2, 1);
        assert(g_nsent == 8 && !hud_sender_ready(&s) && s.st.batches == 2);
        hud_sender_submit(&s, &a1, 2);                    /* a third waits: no more than two overlap */
        assert(g_nsent == 8);
        for (i = 0; i < 4; i++) answer(&s, i, 250);
        assert(s.st.done == 1 && s.st.last_batch_ms == 250 && g_nsent == 12);   /* a2 is the tail now, a1 goes out */
        for (i = 4; i < 12; i++) answer(&s, i, 260);
        assert(s.st.done == 3 && s.st.strays == 0 && hud_sender_idle(&s));
        hud_sender_submit(&s, &a1, 300);
        hud_sender_submit(&s, &a2, 301);
        assert(s.nflight == 8);
        hud_sender_tick(&s, 302 + HUD_ANSWER_TIMEOUT_MS);
        assert(s.st.timeouts == 1 && s.st.failed == 2 && hud_sender_idle(&s) && s.nflight == 0);
    }
}


/* 1 when segments ab and cd cross at a point inside both (touching ends do not count). */
static int crosses(hud_point a, hud_point b, hud_point c, hud_point d) {
    long d1 = (long)(b.x - a.x) * (c.y - a.y) - (long)(b.y - a.y) * (c.x - a.x);
    long d2 = (long)(b.x - a.x) * (d.y - a.y) - (long)(b.y - a.y) * (d.x - a.x);
    long d3 = (long)(d.x - c.x) * (a.y - c.y) - (long)(d.y - c.y) * (a.x - c.x);
    long d4 = (long)(d.x - c.x) * (b.y - c.y) - (long)(d.y - c.y) * (b.x - c.x);
    return ((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0));
}

/* Every polygon in the batch: 3..99 points inside the canvas, no edge crossing another. */
static void check_polygons(const hud_batch *b, const hud_geom *g) {
    unsigned c;
    for (c = 0; c < b->n; c++) {
        const hud_call *k = &b->call[c];
        hud_point q[HUD_MAX_POINTS];
        int n, i, j;
        assert(hud_call_valid(k));
        if (k->fkt != HUD_POLYGON) continue;
        n = k->p[5];
        assert(n >= 3 && n <= HUD_MAX_POINTS);
        for (i = 0; i < n; i++) {
            q[i].x = (int16_t)(k->p[6 + 4 * i] << 8 | k->p[7 + 4 * i]);
            q[i].y = (int16_t)(k->p[8 + 4 * i] << 8 | k->p[9 + 4 * i]);
            assert(q[i].x >= 0 && q[i].x < g->canvas_w && q[i].y >= 0 && q[i].y < g->canvas_h);
        }
        for (i = 0; i < n; i++)
            for (j = i + 2; j < n; j++) {
                if (i == 0 && j == n - 1) continue;
                if (crosses(q[i], q[(i + 1) % n], q[j], q[(j + 1) % n])) {
                    fprintf(stderr, "polygon %u crosses itself at edges %d and %d (n %d, canvas %dx%d, big %d)\n", c, i, j, n, g->canvas_w, g->canvas_h, g->nav_x0);
                    for (i = 0; i < n; i++) fprintf(stderr, " %d,%d", q[i].x, q[i].y);
                    fprintf(stderr, "\n");
                    assert(0);
                }
            }
    }
}

static void test_text(void) {
    hud_text_style s;
    char o[HUD_TEXT_BYTES];
    hud_text_style_default(&s);
#define STREET(in, want) (hud_text_street(in, &s, o, sizeof o), strcmp(o, want) ? fprintf(stderr, "got [%s] want [%s]\n", o, want), 0 : 1)
    /* abbreviations, the dot cut, house numbers */
    assert(STREET("вулиця Хрещатик", "вул. Хрещатик"));
    assert(STREET("вул. Велика Васильківська", "вул. Велика Васильк."));
    assert(STREET("вул. Сагайдачного, 25", "вул. Сагайдачного 25"));
    assert(STREET("просп. Перемоги, 123-А", "просп. Перем. 123-А"));
    assert(STREET("Hauptstraße 12", "Hauptstr. 12"));
    assert(STREET("Avenue des Champs-Élysées", "Ave des Champs-Élys."));
    assert(STREET("12345678901234567890123", "1234567890123456789."));
    /* invisible, decomposed, typographic and non-BMP characters */
    assert(STREET("вулиця\xC2\xA0\xC2\xAB\xD0\x9A\xD0\xB8\xD1\x96\xCC\x88\xD0\xB2\xD1\x81\xD1\x8C\xD0\xBA\xD0\xB0\xC2\xBB\xE2\x80\x8E  ",
                  "вул. \"Київська\""));
    assert(STREET("\xF0\x9F\x9A\xA7 Обхід", "Обхід"));
    assert(STREET("a\xFF\xFE b\xE0\x80\xAF c\t\n", "a b c"));
    assert(STREET("", ""));
    /* Latin: the national transliteration, capitals, the apostrophe, other scripts dropped */
    s.script = HUD_SCRIPT_LATIN;
    assert(STREET("просп. Берестейський", "prosp. Beresteiskyi"));
    assert(STREET("Згурівка", "Zghurivka"));
    assert(STREET("Мар'янівка", "Marianivka"));
    assert(STREET("Їжакевича", "Yizhakevycha"));
    assert(STREET("ЩОРСА", "SHCHORSA"));
    assert(STREET("Kärntner Straße 1", "Karntner Str. 1"));
    assert(STREET("Λεωφόρος Συγγρού", ""));
    /* every output fits a text area, whatever the input */
    {
        char big[600];
        int i;
        for (i = 0; i < 599; i++) big[i] = (char)(i % 3 == 0 ? 0xD0 : i % 3 == 1 ? 0x96 : ' ');
        big[599] = 0;
        for (s.street_max = 8; s.street_max <= 32; s.street_max++) {
            hud_batch b;
            hud_batch_init(&b);
            assert(hud_text_street(big, &s, o, sizeof o) <= s.street_max && hud_text_utf8(&b, 2, o) == 0);
            s.script = !s.script;
        }
    }
    hud_text_style_default(&s);
#undef STREET
#define DIST(d, arrive, want) (hud_text_distance(d, arrive, &s, o, sizeof o), strcmp(o, want) ? fprintf(stderr, "got [%s] want [%s]\n", o, want), 0 : 1)
    assert(DIST(447, 0, "450 м") && DIST(296, 0, "300 м") && DIST(994, 0, "1.0 км") && DIST(1234, 0, "1.2 км"));
    assert(DIST(12345, 0, "12 км") && DIST(3, 0, "Зараз") && DIST(-5, 0, "Зараз") && DIST(20, 1, "Ви прибули"));
    assert(DIST(80, 1, "80 м"));
    s.lang = HUD_LANG_EN;
    assert(DIST(1234, 0, "1.2 km") && DIST(20, 1, "Arrived"));
    s.lang = HUD_LANG_UK; s.script = HUD_SCRIPT_LATIN;
    assert(DIST(1234, 0, "1.2 km") && DIST(20, 1, "Vy prybuly"));
#undef DIST
}

/* Canvases 0..4 scale the default layout; 5 is the HUD's 120x72 image (82/02 31/32) with the big layout (run 3), 6 the
   same with the HUD's own texts over the image top (run 4 T3). */
#define NCANVAS 7
static void set_canvas(hud_geom *g, int c)
{
    static const int k[][2] = { { 480, 180 }, { 400, 150 }, { 640, 240 }, { 320, 180 }, { 480, 120 }, { 120, 72 }, { 120, 72 } };
    if (c >= 5) hud_geom_big(g, k[c][0], k[c][1]);
    else hud_geom_for_canvas(g, k[c][0], k[c][1]);
    g->nav_text_top = c == 6;
}

/* The lowest polygon top in the batch (canvas px). */
static int top_y(const hud_batch *b) {
    unsigned c;
    int i, t = 1 << 30;
    for (c = 0; c < b->n; c++) {
        const hud_call *k = &b->call[c];
        if (k->fkt != HUD_POLYGON) continue;
        for (i = 0; i < k->p[5]; i++) {
            int y = (int16_t)(k->p[8 + 4 * i] << 8 | k->p[9 + 4 * i]);
            if (y < t) t = y;
        }
    }
    return t;
}

static void test_nav(void) {
    static const char *const k_routes[] = { "ua", "intl" };
    hud_palette pal; hud_geom g; hud_handles h; hud_text_handles th;
    hud_text_style ts;
    hud_nav_info n;
    hud_nav_sent sent;
    hud_batch b;
    int r, c, sc, i, d, shown = 0, worst = 0, k;
    hud_palette_default(&pal); hud_geom_default(&g); hud_handles_default(&h); hud_text_handles_default(&th);
    hud_text_style_default(&ts);
    assert(th.street == 2 && th.distance == 3);

    /* Car probes: the 6WB form of ImageGetSize, and the visible-area grid on every canvas. */
    hud_batch_init(&b);
    assert(hud_get_size_6wb(&b, 0x23) == 0 && b.call[0].len == 3 && b.call[0].p[2] == 1 && hud_call_valid(&b.call[0]));
    for (c = 0; c < NCANVAS; c++) {
        set_canvas(&g, c);
        hud_batch_init(&b);
        assert(hud_grid(&b, 6, &g, 4) == 34);
        check_polygons(&b, &g);
    }
    hud_geom_default(&g);

    /* Every step of both routes, at every distance, on every canvas, in both scripts: fits one batch, and every
       polygon is simple and on the canvas. */
    for (r = 0; r < 2; r++) {
        assert(hud_nav_demo_use(k_routes[r]) == 0);
        for (c = 0; c < NCANVAS; c++)
            for (sc = 0; sc < 2; sc++) {
                set_canvas(&g, c);
                ts.script = sc;
                for (i = 0; i < hud_nav_demo_count(); i++)
                    for (d = 0; d <= 1500; d += 50) {
                        hud_nav_demo_get(i, d, &n);
                        memset(&sent, 0, sizeof sent);
                        hud_batch_init(&b);
                        k = hud_nav_frame(&b, &n, &pal, &g, &h, &th, &ts, &sent, &shown);
                        assert(k > 0 && k <= HUD_MAX_CALLS);
                        check_polygons(&b, &g);
                        if (c == 6) assert(top_y(&b) >= 72 * 58 / 180 - 1);   /* clear of the distance text */
                        if (k > worst) worst = k;
                    }
            }
    }
    printf("worst nav frame: %d calls\n", worst);
    assert(hud_nav_demo_use("ua") == 0 && hud_nav_demo_use("nope") == -1);
    hud_geom_default(&g);
    ts.script = HUD_SCRIPT_NATIVE;

    /* Every manoeuvre, every roundabout exit and angle, every lane combination and highlight stays simple, even all
       six arrows in one lane, on every canvas. */
    hud_nav_demo_get(0, 300, &n);
    for (i = 0; i < 64 * NCANVAS; i++) {
        set_canvas(&g, i % NCANVAS);
        n.man = (enum hud_maneuver)(i % HUD_MAN_COUNT);
        n.exit = i % 8;
        n.exit_angle = (i * 15) % 375;
        n.nlanes = HUD_MAX_LANES;
        for (d = 0; d < HUD_MAX_LANES; d++) {
            n.lane[d].arrows = (uint8_t)((i + d * 11) % 64);
            n.lane[d].kind = (uint8_t)(d % 3);
            n.lane[d].recommended = (uint8_t)(d & 1);
            n.lane[d].use = (uint8_t)((i * 7 + d) % 64);
        }
        memset(&sent, 0, sizeof sent);
        hud_batch_init(&b);
        assert(hud_nav_frame(&b, &n, &pal, &g, &h, &th, &ts, &sent, &shown) > 0);
        check_polygons(&b, &g);
    }
    hud_geom_default(&g);
    n.exit_angle = 0;

    /* First frame: both texts, then clear / draw / switch. The same state again sends nothing. */
    hud_nav_demo_get(0, 500, &n);
    memset(&sent, 0, sizeof sent);
    shown = 0;
    hud_batch_init(&b);
    hud_nav_frame(&b, &n, &pal, &g, &h, &th, &ts, &sent, &shown);
    assert(b.call[0].fkt == HUD_TEXT && hud_call_handle(&b.call[0]) == 2);
    assert(b.call[1].fkt == HUD_TEXT && hud_call_handle(&b.call[1]) == 3);
    assert(b.call[2].fkt == HUD_REMOVE_ALL && last(&b)->fkt == HUD_SWITCH_VISIBILITY && shown == 1);
    hud_batch_init(&b);
    assert(hud_nav_frame(&b, &n, &pal, &g, &h, &th, &ts, &sent, &shown) == 0);
    /* 50 m closer (50 m steps above 300 m): the distance text changes; the bar may or may not move a pixel. */
    n.dist_m = 450;
    hud_batch_init(&b);
    assert(hud_nav_frame(&b, &n, &pal, &g, &h, &th, &ts, &sent, &shown) >= 1 && b.call[0].fkt == HUD_TEXT &&
           hud_call_handle(&b.call[0]) == 3);
    /* A new street resends the street. */
    hud_nav_demo_get(1, 500, &n);
    hud_batch_init(&b);
    hud_nav_frame(&b, &n, &pal, &g, &h, &th, &ts, &sent, &shown);
    assert(b.call[0].fkt == HUD_TEXT && hud_call_handle(&b.call[0]) == 2);
    /* A street far over 32 characters still draws (it used to fail the whole frame). */
    memset(n.street, 'x', sizeof n.street - 1);
    n.street[sizeof n.street - 1] = 0;
    hud_batch_init(&b);
    assert(hud_nav_frame(&b, &n, &pal, &g, &h, &th, &ts, &sent, &shown) > 0);
    /* With texts already in a nearly full batch, the lanes give way instead of the frame failing. */
    hud_nav_demo_use("intl");
    hud_nav_demo_get(0, 300, &n);
    memset(&sent, 0, sizeof sent);
    hud_batch_init(&b);
    for (i = 0; i < 26; i++) hud_fill_color(&b, 9, 1);
    assert(hud_nav_frame(&b, &n, &pal, &g, &h, &th, &ts, &sent, &shown) > 0 && b.n <= HUD_MAX_CALLS);
    hud_nav_demo_use("ua");
    /* Clearing empties both texts. */
    hud_batch_init(&b);
    assert(hud_nav_clear(&b, &th, &sent) == 2 && b.call[0].len == 5 && !sent.have_text);

    /* The demo walks the steps and counts down. */
    hud_nav_demo_at(0, &n);
    assert(n.man == HUD_MAN_LEFT && n.dist_m == n.total_m);
    hud_nav_demo_at(8500, &n);
    assert(n.dist_m == 0);
    hud_nav_demo_at(9000, &n);
    assert(n.man == HUD_MAN_STRAIGHT);
}

static void test_gear(void) {
    /* 4th gear (1.72) at 4000 rpm: 4000 * 2.04 * 60 / (1.72 * 3.15 * 1000) = 90.4 km/h. */
    assert(hud_gear_estimate(4000, 90.4) == 4);
    assert(hud_gear_estimate(4000, 48.6) == 2);
    assert(hud_gear_estimate(4000, 56.5) == 0);   /* between 2nd and 3rd, more than 12% from both */
    assert(hud_gear_estimate(800, 0) == 0);
    assert(hud_gear_estimate(6000, 5) == 0);   /* clutch slip, far from any ratio */

    /* The car's own values (owner 2026-11-18: actual data only; the gear in D/S, nothing in P, R, N). */
    assert(hud_gear_decode(DRIVE_SRC_OBD, 0x0006) == 0);      /* [car] P */
    assert(hud_gear_decode(DRIVE_SRC_OBD, 0x0105) == 0);      /* not seen yet: nothing, not a guess */
    assert(hud_gear_decode(DRIVE_SRC_CDS, 0x0106) == 0 && hud_gear_decode(DRIVE_SRC_OBD, -1) == 0);
    assert(hud_mode_decode(DRIVE_SRC_CDS, 2) == HUD_MODE_UNKNOWN);
    assert(hud_mode_decode(DRIVE_SRC_CDS, 3) == HUD_MODE_COMFORT && hud_mode_decode(DRIVE_SRC_CDS, 4) == HUD_MODE_SPORT &&
           hud_mode_decode(DRIVE_SRC_CDS, 5) == HUD_MODE_SPORT_PLUS && hud_mode_decode(DRIVE_SRC_CDS, 7) == HUD_MODE_ECO_PRO);
    assert(hud_mode_decode(DRIVE_SRC_OBD, 4) == HUD_MODE_UNKNOWN);   /* the source matters */
    assert(hud_gear_decode(DRIVE_SRC_CDS, 3) == 0 && hud_gear_decode(DRIVE_SRC_CDS, 2) == 0 && hud_gear_decode(DRIVE_SRC_CDS, 1) == 0);
    assert(hud_gear_decode(DRIVE_SRC_CDS, 5) == 1 && hud_gear_decode(DRIVE_SRC_CDS, 6) == 2 && hud_gear_decode(DRIVE_SRC_CDS, 12) == 8);
    assert(hud_gear_decode(DRIVE_SRC_CDS, 13) == 0 && hud_gear_decode(DRIVE_SRC_CDS, 4) == 0);   /* never seen: hidden */
    {
        hud_live l;
        struct drive_frame f = { 0, 850, -1, -1, -1, DRIVE_SRC_OBD, 0 };
        hud_live_init(&l);
        assert(hud_live_rpm(&l, 100) == -1 && hud_live_gear(&l, 100) == 0 && hud_live_mode(&l, 100) == HUD_MODE_UNKNOWN);
        assert(hud_live_feed(&l, &f, 1000) == 0 && hud_live_rpm(&l, 1500) == 850);
        f.rpm = -1;
        f.gear_raw = 0x0006;                       /* a gear-only frame keeps the revs */
        hud_live_feed(&l, &f, 1300);
        assert(hud_live_rpm(&l, 1500) == 850 && l.gear_raw == 6 && hud_live_gear(&l, 1500) == 0);
        assert(hud_live_rpm(&l, 1000 + HUD_DRIVE_STALE_MS) == -1);   /* old revs draw nothing */
        f.gear_raw = -1;
        f.mode_raw = 7;
        f.source = DRIVE_SRC_CDS;
        assert(hud_live_feed(&l, &f, 2000) == 1 && hud_live_feed(&l, &f, 2100) == 0);
        assert(hud_live_mode(&l, 2100) == HUD_MODE_ECO_PRO);          /* [car] run 6 */
        f.mode_raw = 6;
        hud_live_feed(&l, &f, 2200);
        assert(hud_live_mode(&l, 2200) == HUD_MODE_UNKNOWN);          /* unmapped: Comfort rules */
        /* CDS revs win over the collector's while fresh; the collector takes over when CDS goes quiet. */
        f.mode_raw = -1;
        f.rpm = 3000;
        hud_live_feed(&l, &f, 3000);
        f.source = DRIVE_SRC_OBD;
        f.rpm = 2800;
        hud_live_feed(&l, &f, 3100);
        assert(hud_live_rpm(&l, 3100) == 3000);
        hud_live_feed(&l, &f, 3000 + HUD_DRIVE_STALE_MS);
        assert(hud_live_rpm(&l, 3000 + HUD_DRIVE_STALE_MS) == 2800);
    }
    /* Owner 2026-11-18: the bar only with the engine running and the gearbox in D/S/M. */
    assert(hud_live_gate(HUD_SCENE_RPM, 820, 1) == HUD_SCENE_RPM);   /* D, standing at idle: before a launch */
    assert(hud_live_gate(HUD_SCENE_RPM, 0, 1) == HUD_SCENE_NONE);    /* engine off */
    assert(hud_live_gate(HUD_SCENE_RPM, 820, 0) == HUD_SCENE_NONE);  /* P, N, R or a value not mapped yet */
    assert(hud_live_gate(HUD_SCENE_RPM, -1, 3) == HUD_SCENE_NONE);   /* no fresh revs */
    assert(hud_live_gate(HUD_SCENE_NAV, 0, 0) == HUD_SCENE_NAV);     /* the nav scene doesn't need the engine */
}


/* The plugin's CDS probe: cds.txt, the UTF-16 values, the frames it sends the worker. */
static void test_cds(void) {
    static const char cfg[] = "# car run 6\nengine.RPMSpeed rpm 50\r\ndriving.gear gear\n  driving.mode\n"
                              "bad.kind wobble\nbad.interval log 99999\ndriving.speedActual kph 0\n\n";
    static const uint16_t json[] = { '{', '"', 'R', 'P', 'M', 'S', 'p', 'e', 'e', 'd', '"', ':', '8', '5', '0', '}', 0 };
    static const uint16_t none[] = { '{', '}', 0xe9, 0 };
    hud_cds_prop p[HUD_CDS_MAX];
    struct drive_frame f;
    char text[HUD_CDS_TEXT];
    long v;
    int n = hud_cds_parse(cfg, sizeof cfg - 1, p, HUD_CDS_MAX);
    assert(n == 4);
    assert(!strcmp(p[0].name, "engine.RPMSpeed") && p[0].kind == HUD_CDS_RPM && p[0].interval == 50);
    assert(!strcmp(p[1].name, "driving.gear") && p[1].kind == HUD_CDS_GEAR && p[1].interval == 100);
    assert(!strcmp(p[2].name, "driving.mode") && p[2].kind == HUD_CDS_LOG);
    assert(p[3].kind == HUD_CDS_KPH && p[3].interval == 0);
    assert(hud_cds_parse(cfg, sizeof cfg - 1, p, 2) == 2);
    assert(hud_cds_value(json, HUD_CDS_TEXT, text, sizeof text, &v) == 16 && v == 850 && !strcmp(text, "{\"RPMSpeed\":850}"));
    assert(hud_cds_value(none, HUD_CDS_TEXT, text, sizeof text, &v) == 3 && v == -1 && !strcmp(text, "{}?"));
    assert(hud_cds_value(json, 4, text, sizeof text, &v) == 4 && v == -1);   /* bounded */
    assert(hud_cds_value(NULL, 8, text, sizeof text, &v) == 0 && v == -1 && !text[0]);
    assert(!hud_cds_frame(HUD_CDS_LOG, 850, 5, &f) && !hud_cds_frame(HUD_CDS_RPM, -1, 5, &f));
    assert(hud_cds_frame(HUD_CDS_RPM, 850, 5, &f) && f.rpm == 850 && f.kph == -1 && f.gear_raw == -1 && f.mode_raw == -1 &&
           f.source == DRIVE_SRC_CDS && f.sampled_ms == 5);
    assert(hud_cds_frame(HUD_CDS_MODE, 3, 5, &f) && f.mode_raw == 3 && f.rpm == -1);
    assert(sizeof(struct drive_frame) == 32);
}

/* The installed app's logic: settings round trip, toggles, when the worker runs, its command line, conflicts. */
static void test_app(void) {
    char dir[] = "/tmp/hudappXXXXXX", buf[1024], path[300], st[128];
    const char *argv[HUD_APP_MAX_ARGS];
    hud_settings s, r;
    int argc, i, found = 0;
    assert(mkdtemp(dir));
    hud_app_load(&s, dir);                                  /* no file: the installed defaults */
    assert(s.sport_bar == 1 && s.always == 1 && s.nav == 1 && hud_app_wants_worker(&s));
    hud_app_toggle(&s, HUD_APP_ALWAYS);
    s.shift_rpm = 6400;
    assert(hud_app_save(&s, dir) == 0);
    hud_app_load(&r, dir);
    assert(r.sport_bar == 1 && r.always == 0 && r.nav == 1 && r.shift_rpm == 6400);
    snprintf(path, sizeof path, "%s/settings.txt.new", dir);
    assert(access(path, F_OK) != 0);                        /* no temporary file left */
    hud_app_toggle(&r, HUD_APP_SPORT_BAR);
    hud_app_toggle(&r, HUD_APP_ALWAYS);                     /* greyed out: no effect while Sport Bar is off */
    assert(r.always == 0);
    hud_app_toggle(&r, HUD_APP_NAV);
    assert(!hud_app_wants_worker(&r));
    hud_app_status(&r, 0, NULL, 0, st, sizeof st);
    assert(!strcmp(st, "Off"));
    hud_app_status(&s, 1, NULL, 0, st, sizeof st);
    assert(strstr(st, "Sport and Sport+"));   /* run 6: the driving mode comes from CDS */
    hud_app_status(&s, 1, "HudTest", 0, st, sizeof st);
    assert(strstr(st, "HudTest"));

    /* The Shift point page: five values, 6400 by default, the label shows the choice. */
    {
        hud_settings b;
        char label[64];
        hud_app_defaults(&b);
        assert(b.blink_rpm == 6400);
        hud_app_set_blink(&b, 0);
        assert(b.blink_rpm == 6200);
        hud_app_set_blink(&b, 4);
        hud_app_set_blink(&b, 5);                               /* not a choice: unchanged */
        hud_app_set_blink(&b, -1);
        assert(b.blink_rpm == 6600);
        hud_app_blink_label(&b, label, sizeof label);
        assert(!strcmp(label, "Gear blinks from 6,600 RPM"));
    }
    argc = hud_app_worker_argv(HUD_APP_DIR, "169.254.21.3", buf, sizeof buf, argv, HUD_APP_MAX_ARGS);
    assert(argc > 10 && argv[argc] == NULL);
    assert(!strcmp(argv[0], HUD_APP_DIR "/hud-sportbar-qnx"));
    for (i = 1; i < argc - 1; i++) {
        if (!strcmp(argv[i], "--palette")) { assert(!strcmp(argv[i + 1], "3,4,3,4,3,4,4")); found |= 1; }
        if (!strcmp(argv[i], "--layout")) { assert(!strcmp(argv[i + 1], "split")); found |= 16; }
        if (!strcmp(argv[i], "--backs")) assert(!strcmp(argv[i + 1], "fill"));   /* the dim track: always visible */
        if (!strcmp(argv[i], "--pipeline")) found |= 32;                          /* car run 6 W5 */
        assert(strcmp(argv[i], "--in-flight") && strcmp(argv[i], "--merge"));   /* settings.txt tunes these live */
        if (!strcmp(argv[i], "--collector")) { assert(!strcmp(argv[i + 1], HUD_APP_DIR "/shift-deck-qnx")); found |= 2; }
        if (!strcmp(argv[i], "--host")) { assert(!strcmp(argv[i + 1], "169.254.21.3")); found |= 4; }
        if (!strcmp(argv[i], "--most-inst")) { assert(!strcmp(argv[i + 1], "2")); found |= 8; }
        assert(strcmp(argv[i], "--text-clear"));
    }
    assert(found == 63);
    assert(hud_app_worker_argv(HUD_APP_DIR, "x", buf, 20, argv, HUD_APP_MAX_ARGS) == -1);

    assert(hud_app_conflict(dir) == NULL);
    snprintf(path, sizeof path, "%s/HudTest", dir);
    assert(mkdir(path, 0700) == 0);
    assert(!strcmp(hud_app_conflict(dir), "HudTest"));
    rmdir(path);
    snprintf(path, sizeof path, "%s/settings.txt", dir);
    remove(path);
    rmdir(dir);
}

/* Even-odd fill of one polygon at pixel centre (px + .5, py + .5). */
static int inside(const hud_point *pt, int n, int px, int py) {
    int i, j, in = 0;
    double x = px + 0.5, y = py + 0.5;
    for (i = 0, j = n - 1; i < n; j = i++)
        if ((pt[i].y > y) != (pt[j].y > y) &&
            x < pt[j].x + (double)(pt[i].x - pt[j].x) * (y - pt[j].y) / (pt[i].y - pt[j].y)) in = !in;
    return in;
}

/* The picture a batch leaves on a w x h canvas: each polygon paints its inside in the pen colour, later over earlier. */
static void raster(const hud_batch *b, int w, int h, unsigned char *px) {
    unsigned c;
    int colour = 0, i, x, y;
    memset(px, 0, (size_t)(w * h));
    for (c = 0; c < b->n; c++) {
        const hud_call *k = &b->call[c];
        hud_point q[HUD_MAX_POINTS];
        if (k->fkt == HUD_FILL_COLOR) colour = k->p[2];
        if (k->fkt != HUD_POLYGON) continue;
        for (i = 0; i < k->p[5]; i++) {
            q[i].x = (int16_t)(k->p[6 + 4 * i] << 8 | k->p[7 + 4 * i]);
            q[i].y = (int16_t)(k->p[8 + 4 * i] << 8 | k->p[9 + 4 * i]);
        }
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
                if (inside(q, k->p[5], x, y)) px[y * w + x] = (unsigned char)colour;
    }
}

/* Car run 6 W1-W3 (IMG_9797..9799): the HUD draws a polygon's edges too, so a join along a bottom line that crosses
   empty space shows as a thin line (it ran from the gear to the RPM). Returns 1 if a horizontal edge has a pixel
   with nothing painted on either side of it, from row y_min down. */
static int bare_join(const hud_batch *b, const unsigned char *px, int w, int h, int y_min) {
    unsigned c;
    int i, n, x, y, x0, x1;
    for (c = 0; c < b->n; c++) {
        const hud_call *k = &b->call[c];
        if (k->fkt != HUD_POLYGON) continue;
        n = k->p[5];
        for (i = 0; i < n; i++) {
            int j = (i + 1) % n;
            int ya = k->p[8 + 4 * i] << 8 | k->p[9 + 4 * i], yb = k->p[8 + 4 * j] << 8 | k->p[9 + 4 * j];
            int xa = k->p[6 + 4 * i] << 8 | k->p[7 + 4 * i], xb = k->p[6 + 4 * j] << 8 | k->p[7 + 4 * j];
            if (ya != yb || ya < y_min) continue;
            y = ya;
            x0 = xa < xb ? xa : xb;
            x1 = xa < xb ? xb : xa;
            for (x = x0; x < x1 && x < w; x++)
                if (!(y > 0 && y - 1 < h && px[(y - 1) * w + x]) && !(y < h && px[y * w + x])) {
                    fprintf(stderr, "bare join: polygon %u, y %d, x %d..%d (at %d)\n", c, y, x0, x1, x);
                    return 1;
                }
        }
    }
    return 0;
}

/* 2026-11-18 split layout: a dim track, the lit fields white, the gear on the left and the RPM on the right under it,
   one polygon per digit. */
static void test_split(void) {
    /* The 3 x 5 cells of each digit, row by row, as the keyhole polygons must fill them. */
    static const char *k_cells[10] = { "111101101101111", "001001001001001", "111001111100111", "111001111001111",
                                       "101101111001001", "111100111001111", "111100111101111", "111001001001001",
                                       "111101111101111", "111101111001111" };
    hud_profile p;
    hud_palette pal;
    hud_geom g;
    hud_handles h;
    hud_anim a;
    hud_batch b;
    hud_point pt[HUD_DIGIT_PTS];
    int d, n, cx, cy, rpm, gear, shown = 0, worst = 0;

    for (d = 0; d <= 9; d++) {
        /* w 30, h 50, t 10: every cell is 10 x 10. */
        n = hud_digit_poly(d, 0, 0, 30, 50, 10, pt);
        assert(n >= 4 && n <= HUD_DIGIT_PTS);
        for (cy = 0; cy < 5; cy++)
            for (cx = 0; cx < 3; cx++) {
                int want = k_cells[d][cy * 3 + cx] == '1', x, y;
                for (y = cy * 10; y < cy * 10 + 10; y++)
                    for (x = cx * 10; x < cx * 10 + 10; x++) assert(inside(pt, n, x, y) == want);
            }
    }
    assert(hud_rpm_shown(0) == 0 && hud_rpm_shown(4224) == 4200 && hud_rpm_shown(4226) == 4250);

    hud_profile_b58(&p);
    p.flash_ms = 0;
    hud_palette_default(&pal);
    pal.base_on = pal.yellow_on = pal.red_on = pal.shift_on = pal.ink = 4;   /* lit like the factory speed */
    pal.base_off = pal.yellow_off = pal.red_off = 3;                          /* the whole track dim */
    pal.backs = HUD_BACKS_FILL;
    hud_handles_default(&h);
    hud_geom_split(&g, 120, 72);
    assert(g.gear_cx - g.gear_h * 6 / 11 / 2 == g.x && g.rpm_x1 == g.x + g.w && g.rpm_h < g.gear_h);

    /* 4th at 4200, merged: 2 pens; the unlit fields and the lit ones are a polygon each, each digit its own (2026-11-18: a
       call answers in ~260 ms, the owner wants the HUD as fresh as the factory speed). */
    steady(&a, 4224, 10000);
    a.gear = 4;
    hud_batch_init(&b);
    n = hud_strip_frame(&b, &p, &pal, &g, &h, &a, &shown);
    assert(count(&b, HUD_FILL_COLOR) == 2 && count(&b, HUD_POLYGON) == 7 && n == 1 + 2 + 7 + 1);
    /* ... and paints exactly what one polygon per outline paints, for every frame of the sweep below. */
    {
        static unsigned char one[120 * 72], merged[120 * 72];
        hud_geom g1 = g;
        hud_batch b1;
        g1.merge_pts = 0;
        for (gear = 0; gear <= 8; gear += 4)
            for (rpm = 0; rpm <= 7600; rpm += 50) {
                int s1 = 0, s2 = 0;
                steady(&a, rpm, 10000);
                a.gear = gear;
                a.peak = rpm + 300;
                hud_batch_init(&b1);
                hud_batch_init(&b);
                assert(hud_strip_frame(&b1, &p, &pal, &g1, &h, &a, &s1) > 0 && hud_strip_frame(&b, &p, &pal, &g, &h, &a, &s2) > 0);
                raster(&b1, 120, 72, one);
                raster(&b, 120, 72, merged);
                assert(!memcmp(one, merged, sizeof one));
                assert(!bare_join(&b, one, 120, 72, g.y + g.h + 1));   /* the digits' row; the bar's joins sit on its base */
                assert(count(&b, HUD_POLYGON) <= 8);
            }
        steady(&a, 4224, 10000);
        a.gear = 4;
        hud_batch_init(&b);
        n = hud_strip_frame(&b, &p, &pal, &g, &h, &a, &shown);
    }
    check_polygons(&b, &g);
    /* The gear's left edge is the strip's, the last digit ends on its right edge. */
    {
        unsigned c, i;
        int x0 = 999, x1 = -1;
        for (c = 0; c < b.n; c++)
            if (b.call[c].fkt == HUD_POLYGON)
                for (i = 0; i < b.call[c].p[5]; i++) {
                    int x = b.call[c].p[6 + 4 * i] << 8 | b.call[c].p[7 + 4 * i], y = b.call[c].p[8 + 4 * i] << 8 | b.call[c].p[9 + 4 * i];
                    if (y > g.y + g.h) { if (x < x0) x0 = x; if (x > x1) x1 = x; }
                }
        assert(x0 == g.x && x1 == g.x + g.w);
    }
    /* Every frame fits a batch with room to spare: about the run-4 frame (~24 calls, ~1.85 s on the car). */
    for (gear = 0; gear <= 8; gear++)
        for (rpm = 0; rpm <= 7600; rpm += 25) {
            steady(&a, rpm, 10000);
            a.gear = gear;
            a.peak = rpm + 300;
            hud_batch_init(&b);
            n = hud_strip_frame(&b, &p, &pal, &g, &h, &a, &shown);
            assert(n > 0 && n <= 12);   /* car run 6: a polygon per digit, so one round needs a window of 12+ */
            check_polygons(&b, &g);
            if (n > worst) worst = n;
        }
    printf("split layout: worst frame %d calls\n", worst);
    /* A new readout is a new frame; the same 50 is not. */
    {
        hud_strip_key k1, k2;
        steady(&a, 3010, 10000); k1 = hud_strip_key_of(&p, &g, &a);
        steady(&a, 3020, 10000); k2 = hud_strip_key_of(&p, &g, &a);
        assert(k1.rpm == 3000 && k2.rpm == 3000);
        steady(&a, 3100, 10000); k2 = hud_strip_key_of(&p, &g, &a);
        assert(k2.rpm == 3100 && !hud_strip_key_equal(&k1, &k2));
    }
}

int main(void) {
    test_proto();
    test_fields();
    test_anim();
    test_frames();
    test_scene();
    test_sender();
    test_app();
    test_text();
    test_nav();
    test_gear();
    test_cds();
    test_split();
    puts("hud tests passed");
    return 0;
}
