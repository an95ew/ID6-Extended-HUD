/* Required Notice: Copyright 2026 nbtevo-carplay-advanced contributors
 * Licensed under the PolyForm Noncommercial License 1.0.0 (https://polyformproject.org/licenses/noncommercial/1.0.0)
 *
 * librghook.so: CarPlay route guidance for the NBT Evo's iAP2 stack (QNX mm-ipod, "Media 1.0.70", 2017), which
 * predates it. Ported from st1ng/nbtevo-carplay-advanced src/rghook.c (plain MOST variant), for gcc 4.4.2 / QNX 6.5:
 * real libc headers, pthread mutexes instead of the __atomic spin locks, no KOMBI / MOST HUD sender (ExtendedHUD's
 * worker is the only HUD sender; it reads the file this writes, src/hud/hud_rg.c).
 *
 * LD_PRELOAD into mm-ipod. Only read()/write() on the iAP2 transport descriptor are touched (found by its first iAP2
 * bytes ff 5a / ff 55). The driver itself is not changed:
 *   outgoing  IdentificationInformation (0x1D01): adds 0x5200/0x5203 to MessagesSentByAccessory, 0x5201/0x5202/0x5204
 *             to MessagesReceivedFromDevice and a RouteGuidanceDisplayComponent (param 30), then fixes lengths and checksums.
 *   inject    StartRouteGuidanceUpdates (0x5200) once both IdentificationAccepted (0x1D02) and AuthenticationSucceeded
 *             (0xAA05) arrived. The packet takes the next link sequence number; every later packet of the driver is
 *             renumbered (+delta) and every acknowledgement the phone sends back is shifted (-delta).
 *   incoming  RouteGuidanceUpdate (0x5201), RouteGuidanceManeuverUpdate (0x5202) and LaneGuidanceInformation (0x5204) are
 *             decoded, and handed to the driver as an empty PowerUpdate (0xAE01, $RGHOOK_SUBST) with the same sequence
 *             number: the 2017 driver leaks the buffer of every unknown message and stopped reading after ~20 of them.
 *   output    $RGHOOK_PPS (default /var/iap/pps/routeguidance), PPS "key::value" lines, .tmp + rename; removed when
 *             mm-ipod starts (one file per CarPlay session). Log: $RGHOOK_LOG (default /var/iap/rghook.log).
 *
 * What the loader needs (NOT done by this repo, it is a persistent system change and needs a plan agreed with the owner):
 *   mm-ipod (/opt/conn/bin/carplay/mm-ipod on hu-omap, started per CarPlay session by Connectivity) must run with this
 *   library first in LD_PRELOAD, and /var/iap/pps must exist. st1ng does it with a ksh wrapper
 *     mkdir -p /var/iap/pps; export LD_PRELOAD=<dir>/librghook.so${LD_PRELOAD:+:$LD_PRELOAD}; exec <copy of mm-ipod> "$@"
 *   linked over the original with a path-manager link (ln -sP, forgotten at reboot) by a boot.sh started from one line
 *   in /bin/start_screen.sh. The library must keep the file name librghook.so (next_sym looks for it in LD_PRELOAD).
 *   Never stop CONN_MOCCA (watchdog reset ~1 min later). Their installer checks mm-ipod's sha256 first.
 *
 * Built by build_hud.sh (out/librghook.so). RGHOOK_HOST_TEST: the hooks are called rghook_read/rghook_write and libc is
 * called directly, so tests/test_hud_rg.c can feed decoded messages on the host. */
#define _POSIX_C_SOURCE 200809L
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef RGHOOK_HOST_TEST
#define RG_READ rghook_read
#define RG_WRITE rghook_write
ssize_t rghook_read(int fd, void *buf, size_t n);
ssize_t rghook_write(int fd, const void *buf, size_t n);
void rghook_test_message(unsigned mid, const unsigned char *p, unsigned n);
void rghook_test_reset(void);
#else
#define RG_READ read
#define RG_WRITE write
#endif

typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
typedef int s32;
typedef short s16;

static ssize_t (*r_read)(int, void *, size_t);
static ssize_t (*r_write)(int, const void *, size_t);

#define RG_COMP_ID 0x0F0E /* our RouteGuidanceDisplayComponent's identifier (unique among the head unit's components) */
#define SYN        0x80
#define ACK        0x40
static int transport_fd = -1, logfd = -2, ctl_sess = -1, inited, injected;
static u32 delta, last_out_seq, last_in_seq;
/* link state (write path and the injection) and the decoded guidance; QNX mutexes inherit priority, so mm-ipod's
   threads of different fixed priorities cannot starve the holder (st1ng saw that with a sched_yield() spin lock) */
static pthread_mutex_t link_mx = PTHREAD_MUTEX_INITIALIZER, st_mx = PTHREAD_MUTEX_INITIALIZER,
                       log_mx = PTHREAD_MUTEX_INITIALIZER;

/* addf: snprintf at b + k, clamped, so k never runs past cap - 1 */
static int addf(char *b, int k, int cap, const char *fmt, ...)
{
    va_list ap;
    int r;
    if (k >= cap - 1) return cap - 1;
    va_start(ap, fmt);
    r = vsnprintf(b + k, (size_t)(cap - k), fmt, ap);
    va_end(ap);
    if (r < 0) return k;
    return k + r >= cap - 1 ? cap - 1 : k + r;
}

#ifndef RGHOOK_HOST_TEST
static void *libc_sym(const char *n)
{
    return dlsym(dlopen("libc.so.3", RTLD_NOW), n);
}
/* write() on the other fds goes to the next one in the chain: QNX libc has no useful RTLD_NEXT, so the next is looked
   up in the LD_PRELOAD libraries after this one (dlsym on each one's handle). */
static void *next_sym(const char *n, void *self)
{
    const char *e = getenv("LD_PRELOAD");
    char path[256];
    int seen = 0;
    while (e && *e) {
        unsigned k = 0;
        void *h, *f;
        while (e[k] && e[k] != ':' && k + 1 < sizeof path) { path[k] = e[k]; k++; }
        path[k] = 0;
        e += k;
        if (*e == ':') e++;
        if (!k) continue;
        if (!seen) {
            if (k >= 12 && !strcmp(path + k - 12, "librghook.so")) seen = 1;
            continue;
        }
        h = dlopen(path, RTLD_NOW);
        f = h ? dlsym(h, n) : NULL;
        if (f && f != self) return f;
    }
    return libc_sym(n);
}
#endif

static void rg_clear(void);
static void init(void)
{
    if (inited) return;
    inited = 1;
#ifdef RGHOOK_HOST_TEST
    r_read = read;
    r_write = write;
#else
    r_read = (ssize_t (*)(int, void *, size_t))libc_sym("read");
    r_write = (ssize_t (*)(int, const void *, size_t))next_sym("write", (void *)write);
#endif
    rg_clear();
}

static void log_text(const char *s)
{
    if (logfd == -2) {   /* opened (and truncated) once */
        pthread_mutex_lock(&log_mx);
        if (logfd == -2) {
            const char *p = getenv("RGHOOK_LOG");
            logfd = open(p ? p : "/var/iap/rghook.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        }
        pthread_mutex_unlock(&log_mx);
    }
    if (logfd >= 0) r_write(logfd, s, strlen(s));
}

/* A switch file ($RGHOOK_CFG, else ExtendedHUD's plugin folder; /var may not be linked yet when mm-ipod starts). */
static int cfg_switch(const char *name)
{
    static const char *const dirs[2] = { "/var/opt/hmi/data/ID5/Plugins/ExtendedHUD",
                                         "/fs/sda1/opt/hmi/data/ID5/Plugins/ExtendedHUD" };
    const char *cfg = getenv("RGHOOK_CFG");
    int i;
    for (i = 0; i < 3; i++) {
        const char *d = i == 0 ? cfg : dirs[i - 1];
        char p[192];
        int fd;
        if (!d || !*d) continue;
        snprintf(p, sizeof p, "%s/rghook_%s", d, name);
        fd = open(p, O_RDONLY);
        if (fd >= 0) { close(fd); return 1; }
    }
    return 0;
}
/* Switch rghook_debug_log: the per-message lines. Read once. */
static int rg_debug_log(void)
{
    static volatile int on = -1;
    if (on < 0) on = cfg_switch("debug_log");
    return on;
}
/* Switch rghook_keep_start_marker: the start marker (type 11) stays even when the phone already lists the next turn. */
static int keep_start_marker(void)
{
    static volatile int on = -1;
    if (on < 0) on = cfg_switch("keep_start_marker");
    return on;
}

static u8 checksum(const u8 *b, u32 n)
{
    u32 s = 0;
    while (n--) s += *b++;
    return (u8)(0x100 - (s & 0xff));
}
static u16 be16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }
static void wbe16(u8 *p, u32 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }

/* ---------------- identification patch ---------------- */
static u32 tlv(u8 *o, u32 id, const void *d, u32 n)
{
    wbe16(o, 4 + n);
    wbe16(o + 2, id);
    if (n) memcpy(o + 4, d, n);
    return 4 + n;
}
static u32 tlv16(u8 *o, u32 id, u32 v)
{
    u8 b[2];
    wbe16(b, v);
    return tlv(o, id, b, 2);
}
static u32 rg_component(u8 *o)
{
    static const char nm[] = "Head-Up Display";
    u8 in[256];
    u32 k = 0;
    k += tlv16(in + k, 0, RG_COMP_ID);
    k += tlv(in + k, 1, nm, sizeof nm);   /* identifier, name (UTF-8, NUL-terminated) */
    k += tlv16(in + k, 2, 0x100);
    k += tlv16(in + k, 3, 0x100);
    k += tlv16(in + k, 4, 0x100);
    k += tlv16(in + k, 5, 0x100);         /* max road / destination / after-road / description lengths */
    k += tlv16(in + k, 6, 6);             /* max guidance maneuver storage */
    k += tlv16(in + k, 7, 0x100);
    k += tlv16(in + k, 8, 6);             /* lane guidance description length, storage */
    return tlv(o, 30, in, k);
}
/* msg = control message (40 40 len id params); returns the new length (written to out) */
static u32 patch_ident(const u8 *msg, u32 n, u8 *out)
{
    static const u16 snd[] = { 0x5200, 0x5203 }, rcv[] = { 0x5201, 0x5202, 0x5204 };
    u32 o = 6, i = 6, have = 0;
    memcpy(out, msg, 6);
    while (i + 4 <= n) {
        u32 ln = be16(msg + i), id = be16(msg + i + 2);
        if (ln < 4 || i + ln > n) break;
        if (id == 30) have = 1;
        if (id == 6 || id == 7) {
            const u16 *a = id == 6 ? snd : rcv;
            u32 na = id == 6 ? 2 : 3, add = 0, j, q;
            memcpy(out + o, msg + i, ln);
            for (j = 0; j < na; j++) {
                u32 dup = 0;
                for (q = 4; q + 1 < ln; q += 2)
                    if (be16(msg + i + q) == a[j]) dup = 1;
                if (!dup) { wbe16(out + o + ln + add, a[j]); add += 2; }
            }
            wbe16(out + o, ln + add);
            o += ln + add;
        } else {
            memcpy(out + o, msg + i, ln);
            o += ln;
        }
        i += ln;
    }
    if (!have) o += rg_component(out + o);
    wbe16(out + 2, o);
    return o;
}

/* ---------------- route guidance decode ---------------- */
struct rg_man { u32 used, type, dist, side, jtype, nang; s32 exit_angle; s16 ang[8]; char road[128], desc[128]; };
struct rg_lane1 { u32 pos, status, na; s32 hl; s16 ang[4]; };
struct rg_lanes { u32 used, idx, n; struct rg_lane1 l[8]; };
static struct {
    u32 route_state, maneuver_state, dist, dist_units, rem, rem_units, eta, cur_idx, has_cur, next_idx, has_next;
    char road[128], dest[128], dist_s[32], rem_s[32], src[64];
    struct rg_man man[32];
    /* lane guidance: RouteGuidanceUpdate 16 LaneGuidanceIndex, 18 LaneGuidanceShowing; 0x5204 sets, by index */
    u32 lane_idx, has_lane_idx, lane_show, has_lane_show;
    struct rg_lanes lanes[8];
} st;

static void cpstr(char *d, u32 cap, const u8 *s, u32 n)
{
    u32 k = 0;
    while (k < n && k + 1 < cap && s[k]) { d[k] = (char)s[k]; k++; }
    d[k] = 0;
}
static u32 uint_be(const u8 *p, u32 n)   /* BE, low 32 bits */
{
    u32 v = 0, i;
    for (i = 0; i < n && i < 4; i++) v = v << 8 | p[n > 4 ? n - 4 + i : i];
    return v;
}
/* The distance in the phone's units, from metres (units 0 km, 1 mi, 2 m, 3 yd, 4 ft). Integer only. */
static void fmt_dist(char *o, u32 cap, u32 m, u32 units)
{
    if (units == 1 || units == 3 || units == 4) {
        u32 ft = (m * 3281 + 500) / 1000, yd = (m * 1094 + 500) / 1000;
        if (units == 3 && yd < 500)
            snprintf(o, cap, "%u yd", yd < 100 ? (yd + 5) / 10 * 10 : (yd + 25) / 50 * 50);
        else if (units != 3 && ft < 1000)
            snprintf(o, cap, "%u ft", ft < 100 ? (ft + 5) / 10 * 10 : (ft + 25) / 50 * 50);
        else {
            u32 t = (m * 10 + 804) / 1609;
            snprintf(o, cap, "%u.%u mi", t / 10, t % 10);
        }
    } else if (m < 1000)
        snprintf(o, cap, "%u m", m < 100 ? (m + 5) / 10 * 10 : (m + 25) / 50 * 50);
    else {
        u32 t = (m + 50) / 100;
        snprintf(o, cap, "%u.%u km", t / 10, t % 10);
    }
}
/* After a reroute the phone's current list is [0, 1]: the start marker (type 11, 0 m) and the first real turn. When
   the car is at the start marker (30 m or less) and the next maneuver is known, that one is shown with its step length
   (st1ng issue #9). Called under st_mx. */
static u32 cur_view(u32 *dist, char *ds, u32 cap)
{
    u32 x = st.cur_idx % 32, nx = st.next_idx % 32, m;
    *dist = st.dist;
    snprintf(ds, cap, "%s", st.dist_s);
    if (!st.has_next || st.next_idx == st.cur_idx || !st.man[x].used || st.man[x].type != 11 || st.dist > 30 ||
        !st.man[nx].used || st.man[nx].type == 11 || keep_start_marker())
        return st.cur_idx;
    m = st.man[nx].dist;
    if (m) { *dist = m; fmt_dist(ds, cap, m, st.dist_units); }
    return st.next_idx;
}
static const char *pps_path(void)
{
    const char *p = getenv("RGHOOK_PPS");
    return p ? p : "/var/iap/pps/routeguidance";
}
static void publish(void)
{
    const char *p = pps_path();
    char tmp[192], b[2048], ds[32];
    u32 cur = st.cur_idx, dist = st.dist, q;
    int k, fd, cap = (int)sizeof b;
    if (st.has_cur) cur = cur_view(&dist, ds, sizeof ds);
    else snprintf(ds, sizeof ds, "%s", st.dist_s);
    k = addf(b, 0, cap,
             "@routeguidance\nroute_state:n:%u\nmaneuver_state:n:%u\ndist_to_maneuver:n:%u\ndist_to_maneuver_units:n:%u\n"
             "dist_to_maneuver_string::%s\ndistance_remaining:n:%u\ndistance_remaining_string::%s\ncurrent_road::%s\n"
             "destination::%s\nsource::%s\n",
             st.route_state, st.maneuver_state, dist, st.dist_units, ds, st.rem, st.rem_s, st.road, st.dest, st.src);
    if (st.has_cur) {
        const struct rg_man *M = &st.man[cur % 32];
        k = addf(b, k, cap, "maneuver_index:n:%u\nmaneuver_type:n:%u\nmaneuver_exit_angle:n:%d\nmaneuver_driving_side:n:%u\n"
                 "maneuver_road::%s\nmaneuver_description::%s\nmaneuver_step_length:n:%u\n",
                 cur, M->type, (int)M->exit_angle, M->side, M->road, M->desc, M->dist);
        k = addf(b, k, cap, "maneuver_junction_type:n:%u\nmaneuver_junction_angles::", M->jtype);
        for (q = 0; q < M->nang; q++) k = addf(b, k, cap, q ? ",%d" : "%d", (int)M->ang[q]);
        k = addf(b, k, cap, "\n");
    }
    /* the lanes the phone shows now, left to right: "status/highlight/angle,angle;..." */
    if (st.has_lane_idx) {
        const struct rg_lanes *G = &st.lanes[st.lane_idx % 8];
        k = addf(b, k, cap, "lane_guidance_index:n:%u\nlane_guidance_showing:n:%u\n", st.lane_idx,
                 st.has_lane_show ? st.lane_show : 1);
        if ((!st.has_lane_show || st.lane_show) && G->used && G->idx == st.lane_idx && G->n) {
            k = addf(b, k, cap, "lanes::");
            for (q = 0; q < G->n; q++) {
                const struct rg_lane1 *L = &G->l[q];
                u32 a;
                k = addf(b, k, cap, "%s%u/%d/", q ? ";" : "", L->status, (int)L->hl);
                for (a = 0; a < L->na; a++) k = addf(b, k, cap, a ? ",%d" : "%d", (int)L->ang[a]);
            }
            k = addf(b, k, cap, "\n");
        }
    }
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    r_write(fd, b, (size_t)k);
    close(fd);
    rename(tmp, p);
}
/* LaneGuidanceInformation (0x5204): 1 LaneGuidanceIndex, 2 LaneInformation (one per lane). Inside one: 0 position (u16),
   1 status (u8), 2 angles (s16 each), 3 highlight angle (s16, 1000 = none). Kept sorted by position, at most 8 lanes. */
static void on_lanes(u32 idx, const u8 *p, u32 n)
{
    struct rg_lanes *G = &st.lanes[idx % 8];
    u32 i = 0, cur = 0xffffffffu, a, c;
    memset(G, 0, sizeof *G);
    G->used = 1;
    G->idx = idx;
    while (i + 4 <= n) {
        u32 ln = be16(p + i), id = be16(p + i + 2);
        if (ln < 4 || i + ln > n) break;
        if (id == 2) {
            const u8 *g = p + i + 4;
            u32 gn = ln - 4, j = 0;
            cur = 0xffffffffu;
            while (j + 4 <= gn) {
                u32 sl = be16(g + j), sid = be16(g + j + 2), dn, q;
                const u8 *d;
                if (sl < 4 || j + sl > gn) break;
                d = g + j + 4;
                dn = sl - 4;
                if (sid == 0 || cur == 0xffffffffu) {
                    if (G->n >= 8) break;
                    cur = G->n++;
                    G->l[cur].hl = 1000;
                    G->l[cur].pos = sid == 0 && dn ? uint_be(d, dn) : cur;
                }
                if (sid == 1 && dn) G->l[cur].status = d[0];
                else if (sid == 2)
                    for (q = 0; q + 2 <= dn && G->l[cur].na < 4; q += 2) G->l[cur].ang[G->l[cur].na++] = (s16)be16(d + q);
                else if (sid == 3 && dn >= 2) G->l[cur].hl = (s16)be16(d);
                j += sl;
            }
        }
        i += ln;
    }
    for (a = 1; a < G->n; a++)
        for (c = a; c && G->l[c - 1].pos > G->l[c].pos; c--) {   /* left to right */
            struct rg_lane1 t = G->l[c];
            G->l[c] = G->l[c - 1];
            G->l[c - 1] = t;
        }
}
static void on_rg_locked(u32 mid, const u8 *p, u32 n)
{
    static u32 logged_state = 0xffffffffu;
    char b[600];
    u32 idx = 0xffffffffu, i = 0;
    int k = addf(b, 0, (int)sizeof b, "RG 0x%04X", mid);
    while (i + 4 <= n) {
        u32 ln = be16(p + i), id = be16(p + i + 2), dn, q;
        const u8 *d;
        int str;
        if (ln < 4 || i + ln > n) break;
        d = p + i + 4;
        dn = ln - 4;
        str = dn > 1 && d[dn - 1] == 0;
        for (q = 0; str && q + 1 < dn; q++)
            if (d[q] < 0x20 && d[q] != '\n') str = 0;
        if (str) k = addf(b, k, (int)sizeof b, " %u=\"%s\"", id, (const char *)d);
        else {
            k = addf(b, k, (int)sizeof b, " %u=", id);
            for (q = 0; q < dn && q < 12; q++) k = addf(b, k, (int)sizeof b, "%02x", d[q]);
        }
        if (mid == 0x5201) {
            switch (id) {
            case 1: st.route_state = uint_be(d, dn); break;
            case 2: st.maneuver_state = uint_be(d, dn); break;
            case 3: cpstr(st.road, sizeof st.road, d, dn); break;
            case 4: cpstr(st.dest, sizeof st.dest, d, dn); break;
            case 5: st.eta = uint_be(d, dn); break;
            case 7: st.rem = uint_be(d, dn); break;
            case 8: cpstr(st.rem_s, sizeof st.rem_s, d, dn); break;
            case 9: st.rem_units = uint_be(d, dn); break;
            case 10: st.dist = uint_be(d, dn); break;
            case 11: cpstr(st.dist_s, sizeof st.dist_s, d, dn); break;
            case 12: st.dist_units = uint_be(d, dn); break;
            case 13:   /* ManeuverCurrentList: u16 each, the first is the current one */
                if (dn >= 2) {
                    st.cur_idx = be16(d);
                    st.has_cur = 1;
                    st.has_next = dn >= 4;
                    st.next_idx = dn >= 4 ? be16(d + 2) : 0;
                }
                break;
            case 16: if (dn >= 1) { st.lane_idx = uint_be(d, dn); st.has_lane_idx = 1; } break;
            case 18: if (dn >= 1) { st.lane_show = uint_be(d, dn) != 0; st.has_lane_show = 1; } break;
            case 19: cpstr(st.src, sizeof st.src, d, dn); break;
            default: break;
            }
        }
        if ((mid == 0x5202 || mid == 0x5204) && id == 1) idx = uint_be(d, dn);
        i += ln;
    }
    if (mid == 0x5204 && idx != 0xffffffffu) on_lanes(idx, p, n);
    if (mid == 0x5202 && idx != 0xffffffffu) {
        struct rg_man *M = &st.man[idx % 32];
        memset(M, 0, sizeof *M);
        M->used = 1;
        i = 0;
        while (i + 4 <= n) {
            u32 ln = be16(p + i), id = be16(p + i + 2), dn, q;
            const u8 *d;
            if (ln < 4 || i + ln > n) break;
            d = p + i + 4;
            dn = ln - 4;
            if (id == 2) cpstr(M->desc, sizeof M->desc, d, dn);
            else if (id == 3) M->type = uint_be(d, dn);
            else if (id == 4) cpstr(M->road, sizeof M->road, d, dn);
            else if (id == 5) M->dist = uint_be(d, dn);
            else if (id == 8) M->side = uint_be(d, dn);   /* DrivingSide: 0 right-hand traffic, 1 left-hand */
            else if (id == 9) M->jtype = uint_be(d, dn);
            else if (id == 10)
                for (q = 0; q + 2 <= dn && M->nang < 8; q += 2) M->ang[M->nang++] = (s16)be16(d + q);
            else if (id == 11) M->exit_angle = (s16)uint_be(d, dn);
            i += ln;
        }
    }
    addf(b, k, (int)sizeof b, "\n");
    if (rg_debug_log()) log_text(b);
    if (mid == 0x5201 && st.route_state != logged_state) {   /* without debug_log: only the route's state changes */
        logged_state = st.route_state;
        if (!rg_debug_log()) {
            snprintf(b, sizeof b, "RG route_state %u\n", st.route_state);
            log_text(b);
        }
    }
    publish();
}
static void on_rg(u32 mid, const u8 *p, u32 n)
{
    pthread_mutex_lock(&st_mx);
    on_rg_locked(mid, p, n);
    pthread_mutex_unlock(&st_mx);
}

/* ---------------- link layer ---------------- */
static void fix_packet(u8 *pk, u32 n)
{
    wbe16(pk + 2, n);
    pk[8] = checksum(pk, 8);
    if (n > 9) pk[n - 1] = checksum(pk + 9, n - 10);
}
static void inject_start(void)   /* under link_mx */
{
    u8 m[64], pk[80];
    u32 k = 6;
    char b[96];
    k += tlv16(m + k, 0, RG_COMP_ID);
    k += tlv(m + k, 1, NULL, 0);
    k += tlv(m + k, 2, NULL, 0);
    k += tlv(m + k, 3, NULL, 0);   /* component; request SourceName, SourceSupportsRouteGuidance, SupportsExitInfo */
    m[0] = 0x40;
    m[1] = 0x40;
    wbe16(m + 2, k);
    wbe16(m + 4, 0x5200);
    memset(pk, 0, sizeof pk);
    pk[0] = 0xff;
    pk[1] = 0x5a;
    pk[4] = ACK;
    pk[5] = (u8)(last_out_seq + 1);
    pk[6] = (u8)last_in_seq;
    pk[7] = (u8)ctl_sess;
    memcpy(pk + 9, m, k);
    fix_packet(pk, 9 + k + 1);
    r_write(transport_fd, pk, 9 + k + 1);
    last_out_seq = (last_out_seq + 1) & 0xff;
    delta = (delta + 1) & 0xff;
    injected = 1;
    snprintf(b, sizeof b, "inject StartRouteGuidanceUpdates seq=%u (delta now %u)\n", last_out_seq, delta);
    log_text(b);
}

static int ident_ok, auth_ok;
static u8 out_buf[70000];
ssize_t RG_WRITE(int fd, const void *pv, size_t n)
{
    const u8 *p = (const u8 *)pv;
    u32 i = 0, o = 0;
    ssize_t r;
    init();
    if (transport_fd < 0 && n >= 2 && p[0] == 0xff && (p[1] == 0x5a || p[1] == 0x55)) {
        char b[64];
        transport_fd = fd;
        snprintf(b, sizeof b, "transport fd %d\n", fd);
        log_text(b);
    }
    if (fd != transport_fd || n > sizeof out_buf - 2048) return r_write(fd, pv, n);
    pthread_mutex_lock(&link_mx);
    while (i < n) {
        if (i + 9 <= n && p[i] == 0xff && p[i + 1] == 0x5a && checksum(p + i, 8) == p[i + 8]) {
            u32 ln = be16(p + i + 2), nl;
            u8 *pk;
            if (ln < 9 || i + ln > n) {
                memcpy(out_buf + o, p + i, n - i);
                o += (u32)(n - i);
                break;
            }
            pk = out_buf + o;
            memcpy(pk, p + i, ln);
            nl = ln;
            if (pk[4] & SYN) {
                char b[64];
                u32 q;
                delta = 0;
                injected = 0;
                ctl_sess = -1;
                ident_ok = auth_ok = 0;
                for (q = 9 + 10; q + 3 <= ln - 1; q += 3)
                    if (pk[q + 1] == 0 && ctl_sess < 0) ctl_sess = pk[q];
                snprintf(b, sizeof b, "link SYN, control session %d\n", ctl_sess);
                log_text(b);
            } else {
                if (ln > 9 + 6 && (int)pk[7] == ctl_sess && pk[9] == 0x40 && pk[10] == 0x40 && be16(pk + 13) == 0x1D01) {
                    static u8 m[8192];
                    char b[96];
                    u32 ml = be16(pk + 11), nm;
                    if (ml > ln - 10) ml = ln - 10;
                    nm = patch_ident(pk + 9, ml, m);
                    memcpy(pk + 9, m, nm);
                    nl = 9 + nm + 1;
                    snprintf(b, sizeof b, "IdentificationInformation patched %u -> %u bytes\n", ml, nm);
                    log_text(b);
                }
                pk[5] = (u8)(pk[5] + delta);
                if (nl > 9) last_out_seq = pk[5];
                fix_packet(pk, nl);
            }
            o += nl;
            i += ln;
        } else
            out_buf[o++] = p[i++];
    }
    r = r_write(fd, out_buf, o);
    pthread_mutex_unlock(&link_mx);
    return r < 0 ? r : (ssize_t)n;
}

/* Incoming bytes go to the driver in the same read(); only a header split across reads is held back. Payload bytes
   are copied aside (pkt_buf) to decode the control message once the packet is complete. */
static u8 held_buf[8192], pkt_buf[70000];
static u32 held_len, pkt_left, pkt_buf_len;
static int pkt_ctl;
static u32 subst_id(void)
{
    static u32 v;
    if (!v) {
        const char *s = getenv("RGHOOK_SUBST");
        v = 0xAE01;
        if (s) {
            u32 x = 0;
            for (; *s; s++) {
                int c = *s;
                int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                if (d < 0) break;
                x = x << 4 | (u32)d;
            }
            if (x) v = x;
        }
    }
    return v;
}
static int is_rg(u32 mid) { return mid == 0x5201 || mid == 0x5202 || mid == 0x5204; }
/* StartRouteGuidanceUpdates waits for both IdentificationAccepted and AuthenticationSucceeded: a real iPhone
   identifies first and authenticates afterwards, and ignored a request sent between the two. */
static void packet_done(void)
{
    if (pkt_ctl && pkt_buf_len >= 7 && pkt_buf[0] == 0x40 && pkt_buf[1] == 0x40) {
        u32 mid = be16(pkt_buf + 4), ml = be16(pkt_buf + 2);
        if (ml > pkt_buf_len - 1) ml = pkt_buf_len - 1;
        if (mid == 0x1D02) { ident_ok = 1; log_text("IdentificationAccepted\n"); }
        else if (mid == 0xAA05) { auth_ok = 1; log_text("AuthenticationSucceeded\n"); }
        else if (mid == 0xAA06) log_text("AuthenticationFailed\n");
        else if (mid == 0x1D03) log_text("IdentificationRejected\n");
        else if (is_rg(mid) && ml >= 6) on_rg(mid, pkt_buf + 6, ml - 6);
    }
    if (ident_ok && auth_ok && !injected) {
        pthread_mutex_lock(&link_mx);
        inject_start();
        pthread_mutex_unlock(&link_mx);
    }
}
static int hold(const u8 *b, u32 i, u32 n)   /* keeps the incomplete start of a packet for the next read */
{
    u32 k = n - i;
    if (k > sizeof held_buf) return 0;
    memcpy(held_buf, b + i, k);
    held_len = k;
    return 1;
}
static u32 scan_in(u8 *b, u32 n)   /* returns how many bytes to hand to the driver */
{
    u32 i = 0;
    while (i < n) {
        u8 *pk;
        u32 ln;
        int ctl;
        if (pkt_left) {
            u32 m = n - i < pkt_left ? n - i : pkt_left;
            if (pkt_buf_len + m <= sizeof pkt_buf) { memcpy(pkt_buf + pkt_buf_len, b + i, m); pkt_buf_len += m; }
            pkt_left -= m;
            i += m;
            if (!pkt_left) packet_done();
            continue;
        }
        if (b[i] != 0xff) { i++; continue; }
        if (i + 1 >= n || (b[i + 1] == 0x5a && i + 9 > n)) {
            if (hold(b, i, n)) return i;
            i++;
            continue;
        }
        if (b[i + 1] != 0x5a || checksum(b + i, 8) != b[i + 8] || be16(b + i + 2) < 9) { i++; continue; }
        pk = b + i;
        ln = be16(pk + 2);
        ctl = (int)pk[7] == ctl_sess && !(pk[4] & SYN) && ln > 9;
        if (ctl && ln >= 9 + 6 + 1) {
            if (i + 15 > n) {   /* need the message id */
                if (hold(b, i, n)) return i;
            } else if (pk[9] == 0x40 && pk[10] == 0x40 && is_rg(be16(pk + 13))) {
                if (i + ln > n) {   /* the whole packet first (small) */
                    if (hold(b, i, n)) return i;
                } else {
                    u32 mid = be16(pk + 13), ml = be16(pk + 11);
                    if (ml > ln - 10) ml = ln - 10;
                    if (ml >= 6) on_rg(mid, pk + 15, ml - 6);
                    if (delta) pk[6] = (u8)(pk[6] - delta);
                    last_in_seq = pk[5];
                    wbe16(pk + 9 + 2, 6);
                    wbe16(pk + 9 + 4, subst_id());
                    fix_packet(pk, 16);   /* -> 40 40 00 06 <subst>, a 16-byte packet */
                    memmove(pk + 16, pk + ln, n - (i + ln));
                    n -= ln - 16;
                    i += 16;
                    continue;
                }
            }
        }
        if (!(pk[4] & SYN)) {
            if (delta) { pk[6] = (u8)(pk[6] - delta); pk[8] = checksum(pk, 8); }   /* acks -> the driver's numbering */
            if (ln > 9) last_in_seq = pk[5];
        }
        pkt_ctl = ctl;
        pkt_buf_len = 0;
        pkt_left = ln > 9 ? ln - 9 : 0;
        i += 9;
    }
    return n;
}
/* The guidance belongs to one phone session: a new mm-ipod starts without the previous file. close() is not hooked:
   mm-ipod calls it while the libraries are still loading (it crashed in st1ng's tests). */
static void rg_clear(void)
{
    pthread_mutex_lock(&st_mx);
    memset(&st, 0, sizeof st);
    pthread_mutex_unlock(&st_mx);
    unlink(pps_path());
}

ssize_t RG_READ(int fd, void *buf, size_t n)
{
    u8 *b = (u8 *)buf;
    init();
    if (fd != transport_fd || transport_fd < 0) return r_read(fd, buf, n);
    for (;;) {
        u32 h = held_len, out;
        ssize_t k;
        if (h >= n) h = 0;
        if (h) { memcpy(b, held_buf, h); held_len = 0; }
        k = r_read(fd, b + h, n - h);
        if (k <= 0) {   /* no new data: say so, keep the held bytes */
            if (h) { memcpy(held_buf, b, h); held_len = h; }
            return k;
        }
        out = scan_in(b, h + (u32)k);
        if (out) return (ssize_t)out;
    }
}

#ifdef RGHOOK_HOST_TEST
void rghook_test_message(unsigned mid, const unsigned char *p, unsigned n)
{
    init();
    logfd = -1;
    on_rg(mid, p, n);
}
void rghook_test_reset(void)
{
    init();
    rg_clear();
}
#endif
