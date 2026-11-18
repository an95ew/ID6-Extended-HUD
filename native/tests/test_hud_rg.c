/* CarPlay route guidance: the routeguidance file -> hud_nav_info (src/hud/hud_rg.c), and librghook's writer
   (src/rghook/rghook.c, RGHOOK_HOST_TEST) fed iAP2 messages built as the phone sends them, read back by the parser. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE   /* mkdtemp on macOS */
#include "hud/hud_rg.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void rghook_test_message(unsigned mid, const unsigned char *p, unsigned n);
void rghook_test_reset(void);

static void parse(hud_rg *r, const char *text)
{
    assert(hud_rg_parse(r, text, strlen(text)) == 0);
}

/* A right turn with three lanes, as rghook.c's publish() writes it. */
static const char k_turn[] =
    "@routeguidance\nroute_state:n:1\nmaneuver_state:n:0\ndist_to_maneuver:n:450\ndist_to_maneuver_units:n:2\n"
    "dist_to_maneuver_string::450\ndistance_remaining:n:5200\ndistance_remaining_string::5.2\n"
    "current_road::Khreshchatyk St\ndestination::Home\nsource::Maps\n"
    "maneuver_index:n:3\nmaneuver_type:n:2\nmaneuver_exit_angle:n:90\nmaneuver_driving_side:n:0\n"
    "maneuver_road::Bohdana Khmelnytskoho St\nmaneuver_description::Turn right onto Bohdana Khmelnytskoho St\n"
    "maneuver_step_length:n:450\nmaneuver_junction_type:n:0\nmaneuver_junction_angles::-90,0,90\n"
    "lane_guidance_index:n:1\nlane_guidance_showing:n:1\nlanes::0/1000/0;0/1000/0;2/90/0,90\n";

static void test_turn(void)
{
    hud_rg r;
    parse(&r, k_turn);
    assert(r.route_state == 1 && r.has_maneuver && r.active && !r.hold);
    assert(r.index == 3 && r.type == 2 && r.angle == 90 && r.side == 0);
    assert(r.nav.man == HUD_MAN_RIGHT && r.nav.exit == 0 && r.nav.exit_angle == 0);
    assert(r.nav.dist_m == 450 && r.nav.total_m == 450);
    assert(!strcmp(r.nav.street, "Bohdana Khmelnytskoho St"));
    assert(!strcmp(r.dist_text, "450 m"));
    assert(r.nav.nlanes == 3);
    assert(r.nav.lane[0].arrows == HUD_LANE_STRAIGHT && !r.nav.lane[0].recommended);
    assert(r.nav.lane[2].arrows == (HUD_LANE_STRAIGHT | HUD_LANE_RIGHT) && r.nav.lane[2].recommended);
    assert(r.nav.lane[2].use == HUD_LANE_RIGHT && r.nav.lane[2].kind == HUD_LANE_CAR);
}

/* Roundabout, third exit, leaving to the left; no maneuver road, so the current road is shown. */
static void test_roundabout(void)
{
    hud_rg r;
    parse(&r, "@routeguidance\nroute_state:n:1\ndist_to_maneuver:n:1200\ndist_to_maneuver_units:n:0\n"
              "dist_to_maneuver_string::1.2 km\ncurrent_road::Velyka Vasylkivska St\nmaneuver_index:n:5\n"
              "maneuver_type:n:30\nmaneuver_exit_angle:n:-90\nmaneuver_driving_side:n:0\nmaneuver_road::\n");
    assert(r.active && r.nav.man == HUD_MAN_ROUNDABOUT && r.nav.exit == 3 && r.nav.exit_angle == 270);
    assert(!strcmp(r.nav.street, "Velyka Vasylkivska St") && !strcmp(r.dist_text, "1.2 km") && r.nav.nlanes == 0);
    /* first right, straight over, U-turn; no angle: hud_nav goes by the exit number */
    assert(hud_rg_roundabout_angle(90) == 90 && hud_rg_roundabout_angle(0) == 0 && hud_rg_roundabout_angle(45) == 135);
    assert(hud_rg_roundabout_angle(180) == 360 && hud_rg_roundabout_angle(-180) == 360 && hud_rg_roundabout_angle(-135) == 315);
    parse(&r, "@routeguidance\nroute_state:n:1\nmaneuver_index:n:1\nmaneuver_type:n:19\nmaneuver_exit_angle:n:0\n");
    assert(r.nav.man == HUD_MAN_ROUNDABOUT && r.nav.exit == 0 && r.nav.exit_angle == 345);
}

static void test_maneuvers(void)
{
    static const struct { unsigned type; int angle, side; enum hud_maneuver man; } k[] = {
        { 1, -90, 0, HUD_MAN_LEFT },          { 20, 0, 0, HUD_MAN_LEFT },          { 21, 0, 0, HUD_MAN_RIGHT },
        { 47, 0, 0, HUD_MAN_SHARP_LEFT },     { 48, 0, 0, HUD_MAN_SHARP_RIGHT },   { 49, 0, 0, HUD_MAN_SLIGHT_LEFT },
        { 50, 0, 0, HUD_MAN_SLIGHT_RIGHT },   { 51, 0, 0, HUD_MAN_STRAIGHT },      { 13, 0, 0, HUD_MAN_KEEP_LEFT },
        { 52, 0, 0, HUD_MAN_KEEP_LEFT },      { 14, 0, 0, HUD_MAN_KEEP_RIGHT },    { 53, 0, 0, HUD_MAN_KEEP_RIGHT },
        { 22, 0, 0, HUD_MAN_EXIT_LEFT },      { 23, 0, 0, HUD_MAN_EXIT_RIGHT },    { 8, -30, 0, HUD_MAN_EXIT_LEFT },
        { 8, 30, 1, HUD_MAN_EXIT_RIGHT },     { 8, 0, 0, HUD_MAN_EXIT_RIGHT },     { 8, 0, 1, HUD_MAN_EXIT_LEFT },
        { 9, 0, 0, HUD_MAN_MERGE },           { 4, 0, 0, HUD_MAN_UTURN },          { 18, 0, 0, HUD_MAN_UTURN },
        { 26, 0, 0, HUD_MAN_UTURN },          { 11, 0, 0, HUD_MAN_DEPART },        { 12, 0, 0, HUD_MAN_ARRIVE },
        { 10, 0, 0, HUD_MAN_ARRIVE },         { 24, 0, 0, HUD_MAN_ARRIVE },        { 25, 0, 0, HUD_MAN_ARRIVE },
        { 27, 0, 0, HUD_MAN_ARRIVE },         { 15, 0, 0, HUD_MAN_STRAIGHT },      { 6, 90, 0, HUD_MAN_ROUNDABOUT },
        { 7, 0, 0, HUD_MAN_ROUNDABOUT },      { 28, 0, 0, HUD_MAN_ROUNDABOUT },    { 46, 0, 0, HUD_MAN_ROUNDABOUT },
        /* by the angle: no turn, straight, follow road, an unknown type */
        { 0, 0, 0, HUD_MAN_STRAIGHT },        { 3, 10, 0, HUD_MAN_STRAIGHT },      { 5, -40, 0, HUD_MAN_SLIGHT_LEFT },
        { 5, 100, 0, HUD_MAN_RIGHT },         { 0, -150, 0, HUD_MAN_SHARP_LEFT },  { 99, 175, 0, HUD_MAN_UTURN },
    };
    size_t i;
    int exit;
    for (i = 0; i < sizeof k / sizeof k[0]; i++) {
        enum hud_maneuver m = hud_rg_maneuver(k[i].type, k[i].angle, k[i].side, &exit);
        if (m != k[i].man) fprintf(stderr, "type %u angle %d: %d, want %d\n", k[i].type, k[i].angle, m, k[i].man);
        assert(m == k[i].man);
    }
    hud_rg_maneuver(28, 0, 0, &exit);
    assert(exit == 1);
    hud_rg_maneuver(46, 0, 0, &exit);
    assert(exit == 19);
    hud_rg_maneuver(6, 0, 0, &exit);
    assert(exit == 0);
}

static void test_lanes(void)
{
    hud_rg r;
    int a[3] = { -90, 0, 175 };
    assert(hud_rg_lane_arrows(a, 3) == (HUD_LANE_LEFT | HUD_LANE_STRAIGHT | HUD_LANE_UTURN));
    assert(hud_rg_lane_arrows(NULL, 0) == HUD_LANE_STRAIGHT);
    a[0] = 45; a[1] = -45;
    assert(hud_rg_lane_arrows(a, 2) == (HUD_LANE_SLIGHT_RIGHT | HUD_LANE_SLIGHT_LEFT));
    /* eight lanes, the right two to take: the six shown keep them */
    parse(&r, "@routeguidance\nroute_state:n:1\nmaneuver_index:n:0\nmaneuver_type:n:23\n"
              "lanes::0/1000/-90;0/1000/0;0/1000/0;0/1000/0;0/1000/0;0/1000/0;1/1000/0,45;2/45/45\n");
    assert(r.nav.man == HUD_MAN_EXIT_RIGHT && r.nav.nlanes == HUD_MAX_LANES);
    assert(r.nav.lane[5].recommended && r.nav.lane[5].arrows == HUD_LANE_SLIGHT_RIGHT && r.nav.lane[5].use == 0);
    assert(r.nav.lane[4].recommended && r.nav.lane[4].use == 0);
    assert(r.nav.lane[0].arrows == HUD_LANE_STRAIGHT && !r.nav.lane[0].recommended);   /* the left-turn lane dropped */
    /* a lane to take with a U-turn and a left: the highlight picks the U-turn */
    parse(&r, "@routeguidance\nroute_state:n:1\nmaneuver_index:n:0\nmaneuver_type:n:4\nlanes::2/-180/-90,-180;0/1000/0\n");
    assert(r.nav.nlanes == 2 && r.nav.lane[0].arrows == (HUD_LANE_LEFT | HUD_LANE_UTURN) && r.nav.lane[0].use == HUD_LANE_UTURN);
}

/* Arrival, loading in between routes, the route ended, and rubbish. */
static void test_states(void)
{
    hud_rg r;
    hud_rg_track t;
    hud_rg_track_init(&t);
    parse(&r, k_turn);
    assert(hud_rg_track_update(&t, &r) == 1 && t.route_active && t.nav.total_m == 450);
    /* closer to the same turn: the bar keeps its start */
    parse(&r, "@routeguidance\nroute_state:n:1\ndist_to_maneuver:n:120\nmaneuver_index:n:3\nmaneuver_type:n:2\n");
    assert(hud_rg_track_update(&t, &r) == 1 && t.nav.dist_m == 120 && t.nav.total_m == 450);
    assert(hud_rg_track_update(&t, &r) == 0);
    /* the next maneuver: a new start */
    parse(&r, "@routeguidance\nroute_state:n:1\ndist_to_maneuver:n:300\nmaneuver_index:n:4\nmaneuver_type:n:12\n"
              "dist_to_maneuver_string::0.2\ndist_to_maneuver_units:n:1\n");
    assert(hud_rg_track_update(&t, &r) == 1 && t.nav.man == HUD_MAN_ARRIVE && t.nav.total_m == 300);
    assert(!strcmp(r.dist_text, "0.2 mi"));
    /* loading / locating: the last picture stays */
    parse(&r, "@routeguidance\nroute_state:n:3\n");
    assert(r.hold && !r.active && hud_rg_track_update(&t, &r) == 0 && t.route_active);
    /* arrived (state 2): the route ends */
    parse(&r, "@routeguidance\nroute_state:n:2\nmaneuver_index:n:4\nmaneuver_type:n:12\n");
    assert(!r.active && hud_rg_track_update(&t, &r) == 1 && !t.route_active);
    /* hold without a route before it shows nothing */
    parse(&r, "@routeguidance\nroute_state:n:4\n");
    assert(hud_rg_track_update(&t, &r) == 0 && !t.route_active);
    /* active state but no maneuver yet: nothing to show */
    parse(&r, "@routeguidance\nroute_state:n:1\ncurrent_road::A\n");
    assert(!r.active && r.nav.dist_m == 0);
    /* no file: no route */
    parse(&r, k_turn);
    hud_rg_track_update(&t, &r);
    assert(hud_rg_track_update(&t, NULL) == 1 && !t.route_active);
    assert(hud_rg_parse(&r, "hello\nworld\n", 12) == -1);
    assert(hud_rg_parse(&r, "", 0) == -1);
    /* CRLF and a buffer without a NUL */
    parse(&r, "@routeguidance\r\nroute_state:n:6\r\nmaneuver_index:n:0\r\nmaneuver_type:n:1\r\ncurrent_road::X Rd\r\n");
    assert(r.active && r.nav.man == HUD_MAN_LEFT && !strcmp(r.nav.street, "X Rd"));
    assert(hud_rg_parse(&r, k_turn, 40) == 0 && r.route_state == 1 && !r.has_maneuver);
}

/* ---- librghook's writer, fed iAP2 messages ---- */

static unsigned tlv(unsigned char *o, unsigned id, const void *d, unsigned n)
{
    o[0] = (unsigned char)((n + 4) >> 8); o[1] = (unsigned char)(n + 4);
    o[2] = (unsigned char)(id >> 8); o[3] = (unsigned char)id;
    if (n) memcpy(o + 4, d, n);
    return n + 4;
}
static unsigned tlv16(unsigned char *o, unsigned id, int v)
{
    unsigned char b[2] = { (unsigned char)((unsigned)v >> 8), (unsigned char)v };
    return tlv(o, id, b, 2);
}
static unsigned tlv8(unsigned char *o, unsigned id, unsigned v)
{
    unsigned char b = (unsigned char)v;
    return tlv(o, id, &b, 1);
}
static unsigned tlvs(unsigned char *o, unsigned id, const char *s)
{
    return tlv(o, id, s, (unsigned)strlen(s) + 1);
}
static unsigned lane(unsigned char *o, int pos, unsigned status, const short *ang, int na, int hl)
{
    unsigned char in[64], an[8];
    unsigned k = 0;
    int i;
    for (i = 0; i < na; i++) { an[2 * i] = (unsigned char)((unsigned short)ang[i] >> 8); an[2 * i + 1] = (unsigned char)ang[i]; }
    k += tlv16(in + k, 0, pos);
    k += tlv8(in + k, 1, status);
    k += tlv(in + k, 2, an, (unsigned)(2 * na));
    if (hl != 1000) k += tlv16(in + k, 3, hl);
    return tlv(o, 2, in, k);
}

static void read_back(hud_rg_file *f)
{
    assert(hud_rg_file_poll(f) == 1 && f->present);
}

static void test_writer(void)
{
    char dir[] = "/tmp/hud-rg-XXXXXX", path[64];
    unsigned char m[512];
    unsigned k;
    hud_rg_file f;
    static const short l_straight[1] = { 0 }, l_right[2] = { 0, 90 };
    assert(mkdtemp(dir));
    snprintf(path, sizeof path, "%s/routeguidance", dir);
    setenv("RGHOOK_PPS", path, 1);
    setenv("RGHOOK_CFG", dir, 1);
    rghook_test_reset();
    hud_rg_file_init(&f, path);
    assert(hud_rg_file_poll(&f) == 0 && !f.present);                 /* no file: no route, not an error */

    /* 0x5202: maneuver 0 = turn right onto Oak Ave, 450 m step */
    k = tlv16(m, 0, 0x0F0E);
    k += tlv16(m + k, 1, 0);
    k += tlvs(m + k, 2, "Turn right onto Oak Ave");
    k += tlv8(m + k, 3, 2);
    k += tlvs(m + k, 4, "Oak Ave");
    k += tlv16(m + k, 5, 450);
    k += tlv8(m + k, 8, 0);
    k += tlv16(m + k, 11, 90);
    rghook_test_message(0x5202, m, k);
    /* 0x5204: lanes for index 1 */
    k = tlv16(m, 0, 0x0F0E);
    k += tlv16(m + k, 1, 1);
    k += lane(m + k, 1, 2, l_right, 2, 90);
    k += lane(m + k, 0, 0, l_straight, 1, 1000);
    rghook_test_message(0x5204, m, k);
    /* 0x5201: active, 430 m, current list [0], lanes 1 showing */
    k = tlv16(m, 0, 0x0F0E);
    k += tlv8(m + k, 1, 1);
    k += tlvs(m + k, 3, "Main St");
    k += tlv16(m + k, 10, 430);
    k += tlvs(m + k, 11, "430");
    k += tlv8(m + k, 12, 2);
    k += tlv16(m + k, 13, 0);
    k += tlv16(m + k, 16, 1);
    k += tlv8(m + k, 18, 1);
    rghook_test_message(0x5201, m, k);
    read_back(&f);
    assert(f.rg.active && f.rg.nav.man == HUD_MAN_RIGHT && f.rg.nav.dist_m == 430 && !strcmp(f.rg.nav.street, "Oak Ave"));
    assert(!strcmp(f.rg.dist_text, "430 m"));
    assert(f.rg.nav.nlanes == 2 && !f.rg.nav.lane[0].recommended);   /* sorted by position: the straight lane left */
    assert(f.rg.nav.lane[1].recommended && f.rg.nav.lane[1].arrows == (HUD_LANE_STRAIGHT | HUD_LANE_RIGHT));
    assert(f.rg.nav.lane[1].use == HUD_LANE_RIGHT);
    assert(hud_rg_file_poll(&f) == 0);   /* unchanged: not reread */

    /* 0x5202: maneuver 1 = roundabout exit 2, angle 0 (straight over); then the list moves on */
    k = tlv16(m, 0, 0x0F0E);
    k += tlv16(m + k, 1, 1);
    k += tlv8(m + k, 3, 29);
    k += tlv16(m + k, 11, 0);
    rghook_test_message(0x5202, m, k);
    k = tlv16(m, 0, 0x0F0E);
    k += tlv8(m + k, 1, 1);
    k += tlv16(m + k, 10, 800);
    k += tlvs(m + k, 11, "800");
    k += tlv16(m + k, 13, 1);
    k += tlv8(m + k, 18, 0);   /* lanes no longer showing */
    rghook_test_message(0x5201, m, k);
    read_back(&f);
    assert(f.rg.nav.man == HUD_MAN_ROUNDABOUT && f.rg.nav.exit == 2 && f.rg.nav.exit_angle == 0);
    assert(!strcmp(f.rg.nav.street, "Main St") && f.rg.nav.nlanes == 0 && f.rg.nav.dist_m == 800);

    /* route ended (state 0): no route */
    k = tlv16(m, 0, 0x0F0E);
    k += tlv8(m + k, 1, 0);
    rghook_test_message(0x5201, m, k);
    read_back(&f);
    assert(f.rg.route_state == 0 && !f.rg.active);

    /* a new mm-ipod removes the file */
    rghook_test_reset();
    assert(hud_rg_file_poll(&f) == 1 && !f.present);
    rmdir(dir);
}

int main(void)
{
    test_turn();
    test_roundabout();
    test_maneuvers();
    test_lanes();
    test_states();
    test_writer();
    puts("hud rg tests passed");
    return 0;
}
