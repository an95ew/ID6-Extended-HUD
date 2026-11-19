/* ExtendedHUD worker (hud-sportbar-qnx), started by libExtendedHUD.so.
 *
 *   --dump [--trace pull|idle|sweep] [--rtt MS] [--seconds S]   host: simulate, print one JSON line per frame
 *   --get-size                                                  car: ask both images for their size, print answers
 *   --probe-sizes A-B                                           car: ImageGetSize in both forms ([h], 6WB [h][01]) for
 *                                                               every instance A..B: which exist on this cluster, how big
 *   --grid [--seconds S]                                        car: 32 markers over the canvas, column = width, row =
 *                                                               height: a photo shows which part of the image is visible
 *   --swatches FIRST [--seconds S]                              car: 16 colour swatches FIRST..FIRST+15
 *     [--swatch-n N [--swatch-pages P] [--swatch-line]]         ... or N big bars over the whole canvas, P pages of
 *                                                               --seconds each (FIRST, FIRST+N, ...), line colour
 *   --draw-parked [--seconds S] [--trace pull]                  car: demo sweep (or a trace, with gear), no engine data
 *   --draw-live --udp-port N [--collector PATH --host IP]       car: RPM and speed from a drive collector
 *
 * --scene nav makes --dump and --draw-parked show a scripted route instead of the Sport Bar: --route ua (Ukrainian
 * lanes, default) or intl (other languages, hostile text, six lanes). Texts: --lang uk|en|de|pl,
 * --text native|latin (ASCII if the HUD has no Cyrillic), --street-max N (8..32 characters).
 * Common: --most-inst N (talk to 82/N; 82/02 may be the HUD on a 6WB car), --canvas WxH, --handles A,B, --static-handle N, --text-handles STREET,DIST,
 * --palette b0,b1,y0,y1,r0,r1,ink, --settings FILE, --log (every call and answer to stderr).
 * --draw-live: --rg-file PATH (default /var/iap/pps/routeguidance, written by librghook.so in mm-ipod) gives the
 * CarPlay route; no file = no route. The nav scene does not wait for engine frames.
 * --layout big (strip and gear over the whole canvas), --layout split (gear left, RPM right under the strip), --flash-ms N (0 = steady), --shift-hold MS (pull trace),
 * --shift-colour N (whole bar in N from the shift point), --backs fill|none|outline (unlit fields),
 * --text-visible (SetVisibility on the text handles: NBT does 210/204 on 6WB 0x1A-0x1C, JayJay3071), --text-clear (big nav
 * below the HUD's texts, no bar; opt-in). Undoing state an earlier run left in the terminal (it outlives us until the HUD
 * sleeps): --hide H,H,... (SetVisibility 0, e.g. 33,34,37,46 after grids), --text-hide (the text handles), --pen-reset
 * (line colour 0 is refused with op F, width 0 accepted).
 * Car modes clear what they drew on exit. */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <spawn.h>

extern char **environ;

#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "drive_frame.h"
#include "hud_most.h"
#include "hud_nav.h"
#include "hud_nav_demo.h"
#include "hud_gear.h"
#include "hud_rg.h"
#include "hud_scene.h"
#include "hud_sender.h"
#include "hud_strip.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

enum mode { M_NONE, M_DUMP, M_SIZE, M_PROBE, M_GRID, M_SWATCH, M_PARKED, M_LIVE };

static volatile sig_atomic_t g_run = 1;
static void on_signal(int s) { (void)s; g_run = 0; }

static struct {
    enum mode mode;
    const char *trace, *settings, *collector, *host, *rg_file;
    int rtt, seconds, udp_port, first, last, log, nav, most_inst, container;
    int swatch_n, swatch_pages, swatch_line, big, trace_set, shift_hold, text_visible, text_hide, pen_reset, nhide;
    int in_flight, merge, gear_every, pipeline;   /* 0: the defaults (HUD_IN_FLIGHT, the layout's merging, the collector's) */
    uint16_t hide[8];
    const char *show;   /* how --grid/--swatches bring the image up: switch, set or both */
    hud_profile prof;
    hud_palette pal;
    hud_geom geom;
    hud_handles hh;
    hud_text_handles th;
    hud_text_style text;
    const char *route;
} o;

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000);
}

/* usleep is not in POSIX 2008, and QNX hides it under _POSIX_C_SOURCE 200809L. */
static void sleep_us(long us)
{
    struct timespec ts = { us / 1000000, us % 1000000 * 1000 };
    nanosleep(&ts, NULL);
}

static int parse_list(const char *s, int *out, int n)
{
    int i;
    char *end;
    for (i = 0; i < n; i++) {
        long v = strtol(s, &end, 10);
        if (end == s || v < 0 || v > 65535) return -1;
        out[i] = (int)v;
        if (i + 1 < n) {
            if (*end != ',') return -1;
            s = end + 1;
        } else if (*end) return -1;
    }
    return 0;
}

static int usage(void)
{
    fputs("usage: hud-worker (--dump | --get-size | --probe-sizes A-B | --grid | --swatches FIRST | --draw-parked |"
          " --draw-live --udp-port N) [--most-inst N]"
          " [--trace pull|idle|sweep] [--rtt MS] [--seconds S] [--canvas WxH] [--handles A,B]"
          " [--scene rpm|nav] [--route ua|intl] [--lang uk|en|de|pl] [--text native|latin] [--street-max N]"
          " [--static-handle N] [--text-handles S,D] [--palette b0,b1,y0,y1,r0,r1,ink] [--settings FILE] [--rg-file PATH]"
          " [--collector PATH --host IP] [--show switch|set|both] [--container N] [--log]"
          " [--swatch-n N --swatch-pages P --swatch-line] [--layout big|split] [--flash-ms N] [--shift-hold MS]"
          " [--shift-colour N] [--backs fill|none|outline] [--text-visible] [--text-clear] [--text-hide] [--pen-reset] [--hide H,H,...]"
          " [--in-flight N] [--pipeline] [--merge PTS] [--gear-every N]\n", stderr);
    return 2;
}

static int parse_args(int argc, char **argv)
{
    int i, v[7];
    hud_profile_b58(&o.prof);
    hud_palette_default(&o.pal);
    hud_geom_default(&o.geom);
    hud_handles_default(&o.hh);
    hud_text_handles_default(&o.th);
    hud_text_style_default(&o.text);
    o.route = "ua";
    o.rg_file = HUD_RG_FILE;
    o.show = "switch";
    o.trace = "pull";
    o.rtt = 10;
    o.seconds = 20;
    o.swatch_pages = 1;
    o.shift_hold = 320;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i], *n = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--dump")) o.mode = M_DUMP;
        else if (!strcmp(a, "--get-size")) o.mode = M_SIZE;
        else if (!strcmp(a, "--grid")) o.mode = M_GRID;
        else if (!strcmp(a, "--draw-parked")) o.mode = M_PARKED;
        else if (!strcmp(a, "--draw-live")) o.mode = M_LIVE;
        else if (!strcmp(a, "--log")) o.log = 1;
        else if (!strcmp(a, "--swatch-line")) o.swatch_line = 1;
        else if (!strcmp(a, "--text-visible")) o.text_visible = 1;
        /* run 5: the 6WB text areas stay visible after T3 and shade our picture: switch them off */
        else if (!strcmp(a, "--text-hide")) o.text_hide = 1;
        /* run 4 T0 left both images' pen at width 3 with a line colour; every later draw got outlines (the run-5 "shade") */
        else if (!strcmp(a, "--pen-reset")) o.pen_reset = 1;
        /* run 4 T1a-c left images 33/34, 37 and 46 visible over ours (the run-5 "shade"): SetVisibility(h, 0) first */
        else if (!strcmp(a, "--hide") && n) {
            char *e; const char *q = n;
            o.nhide = 0;
            while (*q && o.nhide < 8) {
                long h = strtol(q, &e, 10);
                if (e == q || h < 1 || h > 255) return usage();
                o.hide[o.nhide++] = (uint16_t)h;
                q = *e == ',' ? e + 1 : e;
                if (*e && *e != ',') return usage();
            }
            i++;
        }
        else if (!strcmp(a, "--text-clear")) o.geom.nav_text_top = 1;   /* opt-in (run 5 U2): picture below the texts, no bar */
        else if (!n) return usage();
        else if (!strcmp(a, "--swatches") && !parse_list(n, v, 1) && v[0] <= 240) { o.mode = M_SWATCH; o.first = v[0]; i++; }
        else if (!strcmp(a, "--probe-sizes") && sscanf(n, "%d-%d", &v[0], &v[1]) == 2 && v[0] >= 1 && v[1] >= v[0] &&
                 v[1] <= 255 && v[1] - v[0] < 64) { o.mode = M_PROBE; o.first = v[0]; o.last = v[1]; i++; }
        else if (!strcmp(a, "--most-inst") && !parse_list(n, v, 1) && v[0] <= 15) { o.most_inst = v[0]; i++; }
        else if (!strcmp(a, "--trace")) { o.trace = n; o.trace_set = 1; i++; }
        else if (!strcmp(a, "--swatch-n") && !parse_list(n, v, 1) && v[0] >= 1 && v[0] <= 8) { o.swatch_n = v[0]; i++; }
        else if (!strcmp(a, "--swatch-pages") && !parse_list(n, v, 1) && v[0] >= 1 && v[0] <= 32) { o.swatch_pages = v[0]; i++; }
        else if (!strcmp(a, "--layout") && (!strcmp(n, "big") || !strcmp(n, "split"))) { o.big = !strcmp(n, "big") ? 1 : 2; i++; }
        else if (!strcmp(a, "--shift-colour") && !parse_list(n, v, 1) && v[0] <= 255) { o.pal.shift_on = (uint8_t)v[0]; i++; }
        else if (!strcmp(a, "--backs") && (!strcmp(n, "fill") || !strcmp(n, "none") || !strcmp(n, "outline"))) {
            o.pal.backs = !strcmp(n, "none") ? HUD_BACKS_NONE : !strcmp(n, "outline") ? HUD_BACKS_OUTLINE : HUD_BACKS_FILL; i++;
        }
        else if (!strcmp(a, "--flash-ms") && !parse_list(n, v, 1) && v[0] <= 5000) { o.prof.flash_ms = v[0]; i++; }
        else if (!strcmp(a, "--shift-hold") && !parse_list(n, v, 1) && v[0] <= 10000) { o.shift_hold = v[0]; i++; }
        else if (!strcmp(a, "--show") && (!strcmp(n, "switch") || !strcmp(n, "set") || !strcmp(n, "both"))) {
            o.show = n; i++;
        }
        else if (!strcmp(a, "--container") && !parse_list(n, v, 1) && v[0] >= 1 && v[0] <= 255) { o.container = v[0]; i++; }
        else if (!strcmp(a, "--scene") && (!strcmp(n, "nav") || !strcmp(n, "rpm"))) { o.nav = !strcmp(n, "nav"); i++; }
        else if (!strcmp(a, "--route") && !hud_nav_demo_use(n)) { o.route = n; i++; }
        else if (!strcmp(a, "--lang") && hud_lang_parse(n, strlen(n)) >= 0) { o.text.lang = hud_lang_parse(n, strlen(n)); i++; }
        else if (!strcmp(a, "--text") && (!strcmp(n, "native") || !strcmp(n, "latin"))) {
            o.text.script = !strcmp(n, "latin") ? HUD_SCRIPT_LATIN : HUD_SCRIPT_NATIVE; i++;
        }
        else if (!strcmp(a, "--street-max") && !parse_list(n, v, 1) && v[0] >= 8 && v[0] <= HUD_TEXT_UNITS) {
            o.text.street_max = v[0]; i++;
        }
        else if (!strcmp(a, "--static-handle") && !parse_list(n, v, 1) && v[0] >= 1 && v[0] <= 255) {
            o.hh.static_h = (uint16_t)v[0]; o.hh.static_backs = 1; i++;
        }
        else if (!strcmp(a, "--text-handles") && !parse_list(n, v, 2) && v[0] >= 1 && v[0] <= 255 && v[1] >= 1 &&
                 v[1] <= 255) { o.th.street = (uint16_t)v[0]; o.th.distance = (uint16_t)v[1]; i++; }
        else if (!strcmp(a, "--rtt") && !parse_list(n, v, 1) && v[0] <= 2000) { o.rtt = v[0]; i++; }
        /* 2026-11-18 latency: calls awaiting an answer (the factory sends 3), points per merged polygon (0 = one per
           outline), and how often the collector reads the gearbox instead of the DME */
        else if (!strcmp(a, "--in-flight") && !parse_list(n, v, 1) && v[0] >= 1 && v[0] <= HUD_MAX_IN_FLIGHT) { o.in_flight = v[0]; i++; }
        else if (!strcmp(a, "--merge") && !parse_list(n, v, 1) && v[0] <= HUD_MAX_POINTS) { o.merge = v[0] ? v[0] : -1; i++; }
        else if (!strcmp(a, "--pipeline")) o.pipeline = 1;
        else if (!strcmp(a, "--gear-every") && !parse_list(n, v, 1) && v[0] >= 1 && v[0] <= 20) { o.gear_every = v[0]; i++; }
        else if (!strcmp(a, "--seconds") && !parse_list(n, v, 1) && v[0] >= 1 && v[0] <= 3600) { o.seconds = v[0]; i++; }
        else if (!strcmp(a, "--udp-port") && !parse_list(n, v, 1) && v[0] >= 1) { o.udp_port = v[0]; i++; }
        else if (!strcmp(a, "--settings")) { o.settings = n; i++; }
        else if (!strcmp(a, "--rg-file")) { o.rg_file = n; i++; }
        else if (!strcmp(a, "--collector")) { o.collector = n; i++; }
        else if (!strcmp(a, "--host")) { o.host = n; i++; }
        else if (!strcmp(a, "--canvas") && sscanf(n, "%dx%d", &v[0], &v[1]) == 2 && v[0] > 0 && v[1] > 0 &&
                 v[0] <= 4096 && v[1] <= 4096) { hud_geom_for_canvas(&o.geom, v[0], v[1]); i++; }
        else if (!strcmp(a, "--handles") && !parse_list(n, v, 2) && v[0] >= 1 && v[0] <= 255 && v[1] >= 1 &&
                 v[1] <= 255) { o.hh.buf[0] = (uint16_t)v[0]; o.hh.buf[1] = (uint16_t)v[1]; i++; }
        else if (!strcmp(a, "--palette") && !parse_list(n, v, 7)) {
            o.pal.base_off = (uint8_t)v[0]; o.pal.base_on = (uint8_t)v[1]; o.pal.yellow_off = (uint8_t)v[2];
            o.pal.yellow_on = (uint8_t)v[3]; o.pal.red_off = (uint8_t)v[4]; o.pal.red_on = (uint8_t)v[5];
            o.pal.ink = (uint8_t)v[6]; i++;
        }
        else return usage();
    }
    if (o.mode == M_NONE || (o.mode == M_LIVE && !o.udp_port) || (o.collector && !o.host)) return usage();
    if (o.swatch_n && o.first + o.swatch_n * o.swatch_pages > 256) return usage();
    if (o.big == 1) hud_geom_big(&o.geom, o.geom.canvas_w, o.geom.canvas_h);
    if (o.big == 2) hud_geom_split(&o.geom, o.geom.canvas_w, o.geom.canvas_h);
    if (o.merge) o.geom.merge_pts = o.merge < 0 ? 0 : o.merge;
    return 0;
}

/* ---- traces: raw RPM and speed at time t ---- */

static const double k_ratio[6] = { 0, 5.0, 3.2, 2.143, 1.72, 1.314 };

static void trace_at(const char *name, uint32_t t, double *rpm, double *kph)
{
    if (!strcmp(name, "idle")) {
        uint32_t c = t % 3200;
        *rpm = c < 260 ? 760 + 2500 * sin(M_PI * c / 520.0) : 760 + 15 * sin(t / 90.0);
        *kph = 0;
    } else if (!strcmp(name, "sweep")) {
        double ph = (t % 8000) / 8000.0;
        *rpm = 800 + 6300 * (ph < .5 ? ph * 2 : 2 - ph * 2);
        *kph = 0;
    } else {
        /* Full-throttle pull 1st to 5th; the driver upshifts 320 ms after the shift point while the engine keeps
           revving toward the redline (B58: 7000), coast, repeat. */
        static const double rate[6] = { 0, 9000, 6200, 4300, 3300, 2500 };
        static uint32_t last = 0, hold = 0;
        static double r = 1500;
        static int gear = 1, phase = 0;
        uint32_t dt = t - last;
        double top = o.prof.shift + 60;
        last = t;
        if (phase == 0) { r += rate[gear] * dt / 1000.0; if (r >= top) { r = top; phase = 1; hold = (uint32_t)o.shift_hold; } }
        else if (phase == 1) {
            r += rate[gear] * dt / 1000.0;
            if (r > o.prof.redline) r = o.prof.redline;
            if (hold > dt) hold -= dt;
            else if (gear < 5) { gear++; r = r * k_ratio[gear] / k_ratio[gear - 1]; phase = 0; }
            else phase = 2;
        } else if (phase == 2) { r -= 2600 * dt / 1000.0; if (r <= 1500) { r = 1500; gear = 1; phase = 3; hold = 900; } }
        else { if (hold > dt) hold -= dt; else phase = 0; }
        *rpm = r;
        *kph = r / (k_ratio[gear] * 3.15) * 2.04 * 60 / 1000;
    }
}

/* ---- host simulation ---- */

typedef struct { uint16_t fkt; uint8_t p[2]; uint32_t due; } sim_answer;
static sim_answer g_sim[HUD_MAX_IN_FLIGHT * 4];
static unsigned g_nsim;
static uint32_t g_sim_now;

static int sim_send(void *u, const hud_call *c)
{
    (void)u;
    if (g_nsim >= sizeof g_sim / sizeof g_sim[0]) return -1;
    g_sim[g_nsim].fkt = c->fkt;
    memcpy(g_sim[g_nsim].p, c->p, 2);
    g_sim[g_nsim].due = g_sim_now + (uint32_t)o.rtt;
    g_nsim++;
    return 0;
}

/* ---- the drive story: what happens in a real car, for --dump --trace drive ---- */

enum ev_kind { EV_MODE, EV_ROUTE, EV_FACTORY, EV_SPORT_BAR, EV_ALWAYS, EV_NAV };

typedef struct { uint32_t at; enum ev_kind kind; int value; const char *text; } story_event;

static const story_event k_story[] = {
    {     0, EV_MODE, HUD_MODE_COMFORT, "Ignition on, Comfort. No route yet: the HUD shows only the factory content" },
    {  4000, EV_ROUTE, 1, "CarPlay: Apple Maps starts a route" },
    { 14000, EV_MODE, HUD_MODE_SPORT, "Driving experience switch: Sport. The Sport Bar; turns don't interrupt it" },
    { 30000, EV_MODE, HUD_MODE_SPORT_PLUS, "Driving experience switch: Sport+" },
    { 48000, EV_ALWAYS, 1, "iDrive menu: Always show Sport Bar, on (Navigation is paused, still ticked)" },
    { 52000, EV_MODE, HUD_MODE_COMFORT, "Driving experience switch: Comfort. Always keeps the Sport Bar" },
    { 58000, EV_FACTORY, 1, "Factory navigation starts guidance: we give the HUD back" },
    { 64000, EV_FACTORY, 0, "Factory navigation finished" },
    { 68000, EV_ALWAYS, 0, "iDrive menu: Always show Sport Bar, off. Navigation comes back" },
    { 76000, EV_NAV, 0, "iDrive menu: Navigation, off" },
    { 80000, EV_MODE, HUD_MODE_ECO_PRO, "Driving experience switch: Eco Pro" },
    { 84000, EV_SPORT_BAR, 0, "iDrive menu: Sport Bar, off" },
    { 86000, EV_NAV, 1, "iDrive menu: Navigation, on. Without the Sport Bar it shows in every mode" },
    { 92000, EV_MODE, HUD_MODE_SPORT_PLUS, "Driving experience switch: Sport+, still navigation" },
    {100000, EV_SPORT_BAR, 1, "iDrive menu: Sport Bar, on. Sport+ shows the bar again" },
    {112000, EV_ROUTE, 0, "CarPlay: route ended" },
};

static const char *mode_name(hud_drive_mode m)
{
    switch (m) {
    case HUD_MODE_COMFORT: return "Comfort";
    case HUD_MODE_SPORT: return "Sport";
    case HUD_MODE_SPORT_PLUS: return "Sport+";
    case HUD_MODE_ECO_PRO: return "Eco Pro";
    default: return "unknown";
    }
}

/* The gear blinks from the Shift point (owner 2026-11-18: only the gear, 6200..6600, default 6400): past it every new frame
   flips the digit, so it blinks as fast as the HUD takes frames. Returns what to draw; *off is the phase. */
static hud_anim blink_view(const hud_anim *a, int blink_rpm, int off)
{
    hud_anim v = *a;
    if (blink_rpm > 0 && a->rpm >= blink_rpm && off) v.gear = 0;
    return v;
}

static int blink_next(const hud_anim *a, int blink_rpm, int off) { return blink_rpm > 0 && a->rpm >= blink_rpm ? !off : 0; }

static const char *scene_name(hud_scene s)
{
    return s == HUD_SCENE_RPM ? "rpm" : s == HUD_SCENE_NAV ? "nav" : "none";
}

/* Cruise in Comfort and Eco Pro: 6th gear, a gentle wave around 1,900 rpm. */
static void cruise_at(uint32_t t, double *rpm, double *kph)
{
    *rpm = 1900 + 250 * sin(t / 2500.0);
    *kph = *rpm / (1.0 * 3.15) * 2.04 * 60 / 1000;
}

static void print_json_string(const char *s)
{
    putchar('"');
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') putchar('\\');
        putchar(*s);
    }
    putchar('"');
}

static void print_batch(uint32_t t, double rpm, int gear, hud_scene scene, const hud_batch *b, const hud_sender *s)
{
    unsigned i, k;
    printf("{\"t\":%u,\"scene\":\"%s\",\"rpm\":%.0f,\"gear\":%d,\"last_ms\":%u,\"calls\":[", t, scene_name(scene), rpm,
           gear, s->st.last_batch_ms);
    for (i = 0; i < b->n; i++) {
        printf("%s\"%03X:", i ? "," : "", b->call[i].fkt);
        for (k = 0; k < b->call[i].len; k++) printf("%02x", b->call[i].p[k]);
        putchar('"');
    }
    puts("]}");
}

static void print_event(uint32_t t, const story_event *e, const hud_settings *set, const hud_state *st)
{
    const char *paused = hud_nav_paused_reason(set);
    printf("{\"t\":%u,\"event\":", t);
    print_json_string(e->text);
    printf(",\"mode\":\"%s\",\"settings\":{\"sport_bar\":%d,\"always\":%d,\"nav\":%d,"
           "\"shift_rpm\":%d},\"route\":%d,\"factory_nav\":%d,\"paused\":",
           mode_name(st->mode), set->sport_bar, set->always, set->nav, set->shift_rpm,
           st->route_active, st->factory_nav);
    if (paused) print_json_string(paused);
    else fputs("null", stdout);
    puts("}");
}

static void apply_event(const story_event *e, hud_settings *set, hud_state *st, uint32_t t, uint32_t *route_start)
{
    switch (e->kind) {
    case EV_MODE: st->mode = (hud_drive_mode)e->value; break;
    case EV_ROUTE: st->route_active = e->value; *route_start = t; break;
    case EV_FACTORY: st->factory_nav = e->value; break;
    case EV_SPORT_BAR: set->sport_bar = e->value; break;
    case EV_ALWAYS: set->always = e->value; break;
    case EV_NAV: set->nav = e->value; break;
    }
}

/* One loop for every dump: the Sport Bar alone, the navigation alone, or the drive story where the menu
   settings, the driving mode and the route decide the scene, as hud_scene_select does in the car. */
static int run_dump(void)
{
    hud_sender s;
    hud_anim a;
    hud_batch b;
    hud_strip_key last_key;
    hud_nav_info nav;
    hud_nav_sent nav_sent;
    hud_settings set;
    hud_state st;
    hud_scene scene, last_scene = HUD_SCENE_NONE;
    int shown = 0, have_key = 0, story = !strcmp(o.trace, "drive"), drawn = 0, blink_off = 0, drawn_gear = 0;
    unsigned next_ev = 0;
    uint32_t t, end = (uint32_t)o.seconds * 1000u, frames = 0, route_start = 0;
    hud_sender_init(&s, sim_send, NULL);
    if (o.in_flight) hud_sender_set_window(&s, (unsigned)o.in_flight);
    hud_sender_set_pipeline(&s, o.pipeline);
    hud_anim_reset(&a);
    hud_settings_default(&set);
    set.text = o.text;
    memset(&st, 0, sizeof st);
    memset(&nav_sent, 0, sizeof nav_sent);
    st.mode = HUD_MODE_COMFORT;
    if (story) end = 118000;
    else if (o.nav) { st.route_active = 1; end = hud_nav_demo_length_ms(); }
    printf("{\"canvas\":[%d,%d],\"handles\":[%u,%u],\"text_handles\":[%u,%u],\"rtt\":%d,\"story\":%d,"
           "\"route\":\"%s\",\"lang\":\"%s\",\"text\":\"%s\",\"street_max\":%d,"
           "\"profile\":[%d,%d,%d,%d],\"palette\":[%u,%u,%u,%u,%u,%u,%u]}\n",
           o.geom.canvas_w, o.geom.canvas_h, o.hh.buf[0], o.hh.buf[1], o.th.street, o.th.distance, o.rtt, story,
           o.route, hud_lang_name(o.text.lang), o.text.script == HUD_SCRIPT_LATIN ? "latin" : "native",
           o.text.street_max,
           o.prof.lo, o.prof.yellow, o.prof.red, o.prof.shift,
           o.pal.base_off, o.pal.base_on, o.pal.yellow_off, o.pal.yellow_on, o.pal.red_off, o.pal.red_on, o.pal.ink);
    if (o.hh.static_backs) {
        hud_batch_init(&b);
        hud_strip_static(&b, &o.prof, &o.pal, &o.geom, &o.hh);
        hud_sender_submit(&s, &b, 0);
        print_batch(0, 0, 0, HUD_SCENE_RPM, &b, &s);
    }
    for (t = 0; t < end; t++) {
        double rpm, kph;
        unsigned i;
        g_sim_now = t;
        while (story && next_ev < sizeof k_story / sizeof k_story[0] && k_story[next_ev].at <= t) {
            apply_event(&k_story[next_ev], &set, &st, t, &route_start);
            print_event(t, &k_story[next_ev], &set, &st);
            next_ev++;
        }
        if (story && st.mode != HUD_MODE_SPORT && st.mode != HUD_MODE_SPORT_PLUS) cruise_at(t, &rpm, &kph);
        else if (o.nav && !story) cruise_at(t, &rpm, &kph);
        else trace_at(story ? "pull" : o.trace, t, &rpm, &kph);
        hud_anim_step(&a, rpm, t);
        a.gear = hud_gear_estimate(rpm, kph);
        if (st.route_active) hud_nav_demo_at(t - route_start, &nav);
        scene = story ? hud_scene_select(&set, &st) : o.nav ? HUD_SCENE_NAV : HUD_SCENE_RPM;
        for (i = 0; i < g_nsim;)
            if (g_sim[i].due <= t) {
                sim_answer x = g_sim[i];
                memmove(&g_sim[i], &g_sim[i + 1], (g_nsim - i - 1) * sizeof g_sim[0]);
                g_nsim--;
                hud_sender_answer(&s, x.fkt, 0xC, x.p, 2, t);
            } else i++;
        hud_sender_tick(&s, t);
        /* A new frame only when the last one is done and the picture changed. */
        if (!hud_sender_ready(&s)) continue;
        hud_batch_init(&b);
        if (scene != last_scene) {   /* leaving a scene: take down what only it draws */
            if (last_scene == HUD_SCENE_NAV) hud_nav_clear(&b, &o.th, &nav_sent);
            if (scene == HUD_SCENE_NONE && drawn) { hud_strip_clear(&b, &o.hh); drawn = 0; }
            have_key = 0;
            nav_sent.have_image = 0;
            last_scene = scene;
        }
        if (scene == HUD_SCENE_RPM) {
            hud_anim v = blink_view(&a, set.blink_rpm, blink_off);
            hud_strip_key k = hud_strip_key_of(&o.prof, &o.geom, &v);
            if (!have_key || !hud_strip_key_equal(&k, &last_key)) {
                if (hud_strip_frame(&b, &o.prof, &o.pal, &o.geom, &o.hh, &v, &shown) > 0) drawn = 1;
                blink_off = blink_next(&a, set.blink_rpm, blink_off);
                drawn_gear = v.gear;
                last_key = k;
                have_key = 1;
            }
        } else if (scene == HUD_SCENE_NAV) {
            if (hud_nav_frame(&b, &nav, &o.pal, &o.geom, &o.hh, &o.th, &set.text, &nav_sent, &shown) > 0) drawn = 1;
        }
        if (b.n) {
            hud_sender_submit(&s, &b, t);
            print_batch(t, a.rpm, scene == HUD_SCENE_RPM ? drawn_gear : a.gear, scene, &b, &s);
            frames++;
        }
    }
    o.seconds = (int)(end / 1000);
    fprintf(stderr, "dump: %u frames in %d s (%.1f fps), batch %u ms last, %u ms max, %u timeouts\n",
            frames, o.seconds, frames / (double)o.seconds, s.st.last_batch_ms, s.st.max_batch_ms, s.st.timeouts);
    return 0;
}

/* ---- car ---- */

static uint32_t g_t0;   /* --log stamps every tx/rx with the ms since the worker started (car run 6 measures latency) */

static void log_call(void *u, const hud_call *c)
{
    char line[1024];
    (void)u;
    hud_call_format(c, line, sizeof line);
    fprintf(stderr, "[hud] tx %s t=%u\n", line, now_ms() - g_t0);
}

static int car_send(void *u, const hud_call *c)
{
    if (o.log) log_call(u, c);
    return hud_most_send(u, c);
}

static void pump(hud_sender *s)
{
    hud_answer ans[16];
    int i, n;
    uint32_t t = now_ms();
    while ((n = hud_most_poll(ans, 16)) > 0)
        for (i = 0; i < n; i++) {
            if (o.log || o.mode == M_SIZE || o.mode == M_PROBE) {
                int k;
                fprintf(stderr, "[hud] rx %03X op %X len %u:", ans[i].fkt, ans[i].op, ans[i].n);
                for (k = 0; k < ans[i].n; k++) fprintf(stderr, " %02x", ans[i].p[k]);
                fprintf(stderr, " t=%u\n", t - g_t0);
            }
            hud_sender_answer(s, ans[i].fkt, ans[i].op, ans[i].p, ans[i].n, t);
        }
    hud_sender_tick(s, t);
}

static void wait_idle(hud_sender *s, int ms)
{
    uint32_t until = now_ms() + (uint32_t)ms;
    while (!hud_sender_idle(s) && (int32_t)(until - now_ms()) > 0) {
        pump(s);
        sleep_us(2000);
    }
}

static int open_udp(int port)
{
    struct sockaddr_in a;
    int fd = socket(AF_INET, SOCK_DGRAM, 0), on = 1;
    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0) { close(fd); return -1; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}

/* spawn, not fork: QNX refuses fork() once a process has threads, and the MOST link has them by now (car run 6: the
   collector never started). diagnostic_worker.h does the same with QNX spawn(). */
static pid_t spawn_collector(void)
{
    char port[8], every[8];
    char *argv[11];
    pid_t pid = -1;
    snprintf(port, sizeof port, "%d", o.udp_port);
    snprintf(every, sizeof every, "%d", o.gear_every ? o.gear_every : 2);
    argv[0] = (char *)o.collector;
    argv[1] = "--host";
    argv[2] = (char *)o.host;
    argv[3] = "--udp-port";
    argv[4] = port;
    /* the real gear from the gearbox; speed is not drawn, so its request goes to the gear instead (2026-11-18) */
    argv[5] = "--gear";
    argv[6] = "--gear-every";
    argv[7] = every;
    argv[8] = "--no-speed";
    argv[9] = NULL;
    if (posix_spawn(&pid, o.collector, NULL, NULL, argv, environ) != 0) pid = -1;
    if (pid < 0) fprintf(stderr, "[hud] collector %s: %s\n", o.collector, strerror(errno));
    return pid;
}

static void read_settings(hud_settings *st)
{
    char buf[512];
    size_t n;
    FILE *f;
    if (!o.settings || !(f = fopen(o.settings, "r"))) return;
    n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    hud_settings_parse(st, buf, n);
}

/* Brings the freshly drawn back buffer up; the image calls come first so it shows complete. */
static void add_show(hud_batch *b)
{
    if (strcmp(o.show, "switch")) hud_set_visibility(b, o.hh.buf[1], 1);
    if (strcmp(o.show, "set")) hud_switch_visibility(b, o.hh.buf[0], o.hh.buf[1]);
    /* On the 6WB the cluster's whole nav picture sits in container 0x17 (JayJay3071/nbt-android-auto). */
    if (o.container) hud_set_visibility(b, (uint16_t)o.container, 1);
}

static int run_car(void)
{
    hud_sender s;
    hud_batch b;
    hud_anim a;
    hud_settings set;
    hud_nav_info nav;
    hud_nav_sent nav_sent;
    hud_strip_key last_key;
    int shown = 0, have_key = 0, drawn = 0, fd = -1, blink_off = 0;
    pid_t child = -1;
    hud_rg_file rgf;
    hud_rg_track rgt;
    uint32_t start, next_stats, next_settings = 0, next_rg = 0;
    /* each value with the time it arrived (0 = never) and its source, as the collector and the plugin's CDS feed them */
    hud_live live;
    int cur_merge = -1, layout_merge = o.geom.merge_pts;
    uint32_t last_sample = 0, lag_sample = 0, lag_last = 0, lag_max = 0;
    unsigned lag_done = 0;
    int lag_wait = 0;
    double rpm = 0, kph = 0;
    enum hud_most_rc rc;
    hud_most_set_instance((uint8_t)o.most_inst);
    rc = hud_most_open(5000);
    if (rc != HUD_MOST_UP) {
        fprintf(stderr, "[hud] MOST open failed: %d (-1 no 82/00 target, -2 no server, -3 no DSI library)\n", (int)rc);
        hud_most_close();
        return 1;
    }
    fprintf(stderr, "[hud] 82/%02X up, host %04X\n", o.most_inst, hud_most_host());
    hud_sender_init(&s, car_send, NULL);
    if (o.in_flight) hud_sender_set_window(&s, (unsigned)o.in_flight);
    hud_sender_set_pipeline(&s, o.pipeline);
    hud_settings_default(&set);
    set.text = o.text;   /* until a settings file says otherwise */
    hud_rg_file_init(&rgf, o.rg_file);
    hud_rg_track_init(&rgt);
    hud_anim_reset(&a);
    hud_live_init(&live);
    memset(&nav, 0, sizeof nav);
    memset(&nav_sent, 0, sizeof nav_sent);
    start = next_stats = g_t0 = now_ms();

    if (o.mode == M_SIZE) {
        hud_batch_init(&b);
        hud_get_size(&b, o.hh.buf[0]);
        hud_get_size(&b, o.hh.buf[1]);
        hud_sender_submit(&s, &b, now_ms());
        wait_idle(&s, 3000);
        fprintf(stderr, "[hud] get-size: %u answers, %u errors, %u timeouts\n", s.st.answers, s.st.errors, s.st.timeouts);
        hud_most_close();
        return s.st.answers ? 0 : 1;
    }
    if (o.mode == M_PROBE) {   /* both forms per instance; an unknown one answers an error or nothing */
        int h = o.first;
        while (h <= o.last && g_run) {
            hud_batch_init(&b);
            for (; h <= o.last && b.n + 2 <= HUD_MAX_CALLS; h++) {
                hud_get_size(&b, (uint16_t)h);
                hud_get_size_6wb(&b, (uint16_t)h);
            }
            hud_sender_submit(&s, &b, now_ms());
            wait_idle(&s, 8000);
        }
        fprintf(stderr, "[hud] probe-sizes %d..%d: %u answers, %u errors, %u timeouts. The rx C04 lines with op C/D carry"
                " [h](01)[w u16][h u16].\n", o.first, o.last, s.st.answers, s.st.errors, s.st.timeouts);
        hud_most_close();
        return s.st.answers ? 0 : 1;
    }
    if (o.mode == M_GRID) {
        hud_batch_init(&b);
        hud_grid(&b, o.hh.buf[1], &o.geom, o.pal.ink);
        add_show(&b);
        hud_sender_submit(&s, &b, now_ms());
        wait_idle(&s, 5000);
        fprintf(stderr, "[hud] grid on %ux%u: %u errors, %u timeouts. Photograph the HUD: columns grow wider to the right,"
                " rows taller downward.\n", o.geom.canvas_w, o.geom.canvas_h, s.st.errors, s.st.timeouts);
        while (g_run && (int32_t)(now_ms() - start) < o.seconds * 1000) { pump(&s); sleep_us(20000); }
        drawn = 1;
    }
    if (o.mode == M_SWATCH && o.swatch_n) {
        int page;
        for (page = 0; page < o.swatch_pages && g_run; page++) {
            int first = o.first + page * o.swatch_n;
            uint32_t t0 = now_ms();
            hud_batch_init(&b);
            hud_swatch_bars(&b, o.hh.buf[1], &o.geom, (uint8_t)first, o.swatch_n, o.swatch_line, o.pal.ink);
            if (page == 0) add_show(&b);
            hud_sender_submit(&s, &b, now_ms());
            wait_idle(&s, 5000);
            fprintf(stderr, "[hud] page %d at %.1f s: %s %d..%d left to right, %u ms, %u errors, %u timeouts\n",
                    page + 1, (now_ms() - start) / 1000.0, o.swatch_line ? "line colours" : "fill colours", first,
                    first + o.swatch_n - 1, s.st.last_batch_ms, s.st.errors, s.st.timeouts);
            while (g_run && (int32_t)(now_ms() - t0) < o.seconds * 1000) { pump(&s); sleep_us(20000); }
        }
        drawn = 1;
    } else if (o.mode == M_SWATCH) {
        hud_batch_init(&b);
        hud_swatches(&b, o.hh.buf[1], &o.geom, (uint8_t)o.first);
        add_show(&b);
        hud_sender_submit(&s, &b, now_ms());
        wait_idle(&s, 5000);
        fprintf(stderr, "[hud] swatches %d..%d: %u ms, %u errors, %u timeouts. Photograph the HUD.\n",
                o.first, o.first + 15, s.st.last_batch_ms, s.st.errors, s.st.timeouts);
        while (g_run && (int32_t)(now_ms() - start) < o.seconds * 1000) { pump(&s); sleep_us(20000); }
        drawn = 1;
    }
    if (o.mode == M_LIVE) {
        if (o.collector) child = spawn_collector();
        fd = open_udp(o.udp_port);
        if (fd < 0) fprintf(stderr, "[hud] UDP port %d: %s\n", o.udp_port, strerror(errno));
    }
    if (o.mode == M_PARKED || o.mode == M_LIVE) {
        if (o.container || (o.nav && o.text_visible) || o.text_hide || o.pen_reset || o.nhide) {
            int k;
            hud_batch_init(&b);
            for (k = 0; k < o.nhide; k++) hud_set_visibility(&b, o.hide[k], 0);
            if (o.pen_reset) {   /* colour first: 0 draws nothing, so the outline is gone even if a width is refused */
                hud_line_color(&b, o.hh.buf[0], 0);
                hud_line_color(&b, o.hh.buf[1], 0);
                hud_line_width(&b, o.hh.buf[0], 0);
                hud_line_width(&b, o.hh.buf[1], 0);
            }
            if (o.container) hud_set_visibility(&b, (uint16_t)o.container, 1);
            if (o.nav && o.text_visible) {
                hud_set_visibility(&b, o.th.street, 1);
                hud_set_visibility(&b, o.th.distance, 1);
            } else if (o.text_hide) {
                hud_set_visibility(&b, o.th.street, 0);
                hud_set_visibility(&b, o.th.distance, 0);
            }
            hud_sender_submit(&s, &b, now_ms());
            wait_idle(&s, 3000);
        }
        if (o.hh.static_backs) {
            hud_batch_init(&b);
            hud_strip_static(&b, &o.prof, &o.pal, &o.geom, &o.hh);
            hud_sender_submit(&s, &b, now_ms());
            wait_idle(&s, 3000);
        }
        while (g_run && (o.mode == M_LIVE || (int32_t)(now_ms() - start) < o.seconds * 1000)) {
            uint32_t t = now_ms();
            hud_scene scene = HUD_SCENE_RPM;
            if (o.mode == M_PARKED && o.nav) {
                hud_nav_demo_at(t - start, &nav);
                scene = HUD_SCENE_NAV;
            } else if (o.mode == M_PARKED) {
                trace_at(o.trace_set ? o.trace : "sweep", t - start, &rpm, &kph);
                a.gear = o.trace_set ? hud_gear_estimate(rpm, kph) : 0;
            } else {
                struct drive_frame f;
                hud_state st;
                while (fd >= 0 && recv(fd, &f, sizeof f, 0) == (ssize_t)sizeof f) {
                    if (f.rpm >= 0) last_sample = (uint32_t)f.sampled_ms;   /* the collector's CLOCK_MONOTONIC, same node */
                    if (hud_live_feed(&live, &f, t))
                        fprintf(stderr, "[hud] driving mode raw %d from %s: %s\n", f.mode_raw,
                                f.source == DRIVE_SRC_CDS ? "CDS" : "OBD", mode_name(hud_live_mode(&live, t)));
                }
                if ((int32_t)(t - next_settings) >= 0) {
                    read_settings(&set);
                    next_settings = t + 1000;
                    /* the latency knobs (car run 6 tunes them in settings.txt); a command-line value wins */
                    if (!o.in_flight && s.window != (unsigned)set.in_flight) {
                        hud_sender_set_window(&s, (unsigned)set.in_flight);
                        fprintf(stderr, "[hud] in flight: %u\n", s.window);
                    }
                    if (!o.merge && set.merge_pts != cur_merge) {
                        cur_merge = set.merge_pts;
                        o.geom.merge_pts = cur_merge < 0 ? layout_merge : cur_merge;
                        have_key = 0;
                        fprintf(stderr, "[hud] merge: %d points\n", o.geom.merge_pts);
                    }
                }
                hud_profile_for_shift(&o.prof, set.shift_rpm);
                memset(&st, 0, sizeof st);
                /* unknown (no source, or a value not mapped yet) counts as Comfort: the bar needs Always */
                st.mode = hud_live_mode(&live, t);
                if ((int32_t)(t - next_rg) >= 0) {   /* the CarPlay route (librghook's file) */
                    int was = rgt.route_active;
                    if (hud_rg_file_poll(&rgf)) hud_rg_track_update(&rgt, rgf.present ? &rgf.rg : NULL);
                    if (was != rgt.route_active)
                        fprintf(stderr, "[hud] CarPlay route %s (route_state %d)\n", rgt.route_active ? "started" : "ended",
                                rgf.present ? rgf.rg.route_state : -1);
                    next_rg = t + 250;
                }
                st.route_active = rgt.route_active;
                if (rgt.route_active) nav = rgt.nav;
                scene = hud_scene_select(&set, &st);
                /* Owner 2026-11-18: the bar only with the engine running and the gearbox in D, S or M, from the car's own
                   values; engine off (0 rpm), P, N, R, a gearbox value not mapped yet or no fresh data: nothing of ours. */
                a.gear = hud_live_gear(&live, t);
                rpm = hud_live_rpm(&live, t);
                scene = hud_live_gate(scene, (int)rpm, a.gear);
                if (rpm < 0) {
                    hud_anim_reset(&a);
                    rpm = 0;
                }
            }
            hud_anim_step(&a, rpm, t);
            pump(&s);
            if (hud_sender_ready(&s)) {
                if (scene == HUD_SCENE_RPM) {
                    hud_anim v = blink_view(&a, set.blink_rpm, blink_off);
                    hud_strip_key k = hud_strip_key_of(&o.prof, &o.geom, &v);
                    if (!have_key || !hud_strip_key_equal(&k, &last_key)) {
                        hud_batch_init(&b);
                        if (hud_strip_frame(&b, &o.prof, &o.pal, &o.geom, &o.hh, &v, &shown) > 0) {
                            hud_sender_submit(&s, &b, t);
                            blink_off = blink_next(&a, set.blink_rpm, blink_off);
                            /* lag: from the revs' sample to this frame's last answer */
                            if (o.mode == M_LIVE && a.have && !lag_wait) { lag_sample = last_sample; lag_done = s.st.done; lag_wait = 1; }
                        }
                        last_key = k;
                        have_key = drawn = 1;
                    }
                } else if (scene == HUD_SCENE_NAV) {
                    hud_batch_init(&b);
                    if (hud_nav_frame(&b, &nav, &o.pal, &o.geom, &o.hh, &o.th, &set.text, &nav_sent, &shown) > 0) {
                        hud_sender_submit(&s, &b, t);
                        drawn = 1;
                    }
                } else if (drawn) {
                    hud_batch_init(&b);
                    hud_strip_clear(&b, &o.hh);
                    hud_sender_submit(&s, &b, t);
                    drawn = have_key = 0;
                }
            }
            if (lag_wait && s.st.done > lag_done) {
                lag_last = now_ms() - lag_sample;
                if (lag_last > lag_max) lag_max = lag_last;
                lag_wait = 0;
            } else if (lag_wait && hud_sender_idle(&s)) lag_wait = 0;   /* that frame failed */
            if ((int32_t)(t - next_stats) >= 0) {
                fprintf(stderr, "[hud] rpm %.0f gear %d: batches %u done %u replaced %u failed %u, calls %u answers %u "
                        "errors %u timeouts %u strays %u, batch %u ms (max %u), lag %u ms (max %u), gear raw %d mode %s\n",
                        a.rpm, a.gear, s.st.batches, s.st.done, s.st.replaced, s.st.failed, s.st.calls, s.st.answers,
                        s.st.errors, s.st.timeouts, s.st.strays, s.st.last_batch_ms, s.st.max_batch_ms, lag_last, lag_max,
                        live.gear_raw, mode_name(hud_live_mode(&live, t)));
                next_stats = t + 2000;
            }
            if (hud_most_alive() != 0) { fprintf(stderr, "[hud] MOST link lost\n"); break; }
            sleep_us(2000);
        }
    }
    if (drawn) {
        wait_idle(&s, 2000);
        hud_batch_init(&b);
        hud_strip_clear(&b, &o.hh);
        if (nav_sent.have_text) hud_nav_clear(&b, &o.th, &nav_sent);
        if (o.nav && o.text_visible) {   /* leave the text areas as we found them (run 5: they shade our picture) */
            hud_set_visibility(&b, o.th.street, 0);
            hud_set_visibility(&b, o.th.distance, 0);
        }
        hud_sender_submit(&s, &b, now_ms());
        wait_idle(&s, 2000);
    }
    if (child > 0) { kill(child, SIGTERM); waitpid(child, NULL, 0); }
    if (fd >= 0) close(fd);
    hud_most_close();
    return 0;
}

int main(int argc, char **argv)
{
    struct sigaction sa;
    if (parse_args(argc, argv)) return 2;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    /* A DSI send has no timeout; bound the whole run as the diagnostic workers do. */
    /* Run 3: the bars page through --seconds each; an alarm of one page killed S0f on page 5, uncleared. */
    if (o.mode != M_DUMP)
        alarm((unsigned)(o.mode == M_LIVE ? 0 : o.seconds * (o.mode == M_SWATCH && o.swatch_n ? o.swatch_pages : 1) + 30));
    return o.mode == M_DUMP ? run_dump() : run_car();
}
