/* Read-only collector for ExtendedHUD: polls DME 0x12 for PID 01 0C (RPM) and 01 0D (speed), and with --gear the
 * gearbox (EGS 0x18, UDS 22 DA 2E),
 * over HSFZ TCP 6801 with automatic fallback to the MOST UDS tunnel, and streams struct drive_frame
 * packets over UDP to 127.0.0.1. Sends only OBD service 01 and UDS 22 (read); never writes to any ECU. */
#define _POSIX_C_SOURCE 200809L
#include "gauge_core.h"
#include "diagnostic_lease.h"
#include "diag_link.h"
#include "drive_frame.h"
#include "most/dsi_client.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static unsigned number(const char *s, unsigned lo, unsigned hi) {
    char *end;
    unsigned long n = strtoul(s, &end, 10);
    if (!*s || *end || n < lo || n > hi) {
        fprintf(stderr, "Invalid number: %s\n", s);
        exit(2);
    }
    return (unsigned)n;
}

/* The raw PID value, -1 when the ECU refused it. Returns 0 read, -1 link lost, -3 skip this frame. */
static int read_live_pid(struct diag_link *link, unsigned char pid, int width, int *value) {
    unsigned char req[] = {0x01, pid}, prefix[] = {0x41, pid}, reply[4];
    unsigned negative;
    int n = diag_link_request(link, 0x12, req, sizeof req, prefix, sizeof prefix, reply, sizeof reply, 250, &negative);
    *value = -1;
    if (n == -1 || n == -3) return n;
    if (n == width + 2) *value = width == 1 ? reply[2] : ((unsigned)reply[2] * 256 + reply[3]) / 4;
    return 0;
}

/* RPM and speed in one OBD request (01 0C 0D, SAE J1979 allows up to six PIDs): one ~260 ms MOST round trip instead
   of two. 1 when the DME answered both, 0 when it answered RPM only or refused (the caller reads speed alone from
   then on), -1/-3 as diag_link_request. */
static int read_rpm_speed(struct diag_link *link, int *rpm, int *kph) {
    unsigned char req[] = {0x01, 0x0c, 0x0d}, prefix[] = {0x41, 0x0c}, reply[8];
    unsigned negative;
    int n = diag_link_request(link, 0x12, req, sizeof req, prefix, sizeof prefix, reply, sizeof reply, 250, &negative);
    *rpm = *kph = -1;
    if (n == -1 || n == -3) return n;
    if (n >= 4) *rpm = ((unsigned)reply[2] * 256 + reply[3]) / 4;
    if (n >= 6 && reply[4] == 0x0d) { *kph = reply[5]; return 1; }
    return 0;
}

/* The gearbox's gear/selector: EGS 0x18, UDS 22 DA 2E. ID6_apps' BEAMer and controls read it as their Park check on
   this car (62 DA 2E 00 06 = P); the other values are mapped from car run 6 (hud_gear.c). Raw = byte 3 << 8 | byte 4.
   Prints every new reply in full, so the run log holds the evidence for the map. */
static int read_gear(struct diag_link *link, int *raw, int *last_logged) {
    unsigned char req[] = {0x22, 0xda, 0x2e}, prefix[] = {0x62, 0xda, 0x2e}, reply[16];
    unsigned negative;
    int n = diag_link_request(link, 0x18, req, sizeof req, prefix, sizeof prefix, reply, sizeof reply, 250, &negative);
    *raw = -1;
    if (n == -1 || n == -3) return n;
    if (n >= 5) *raw = reply[3] << 8 | reply[4];
    if (n >= 5 ? *raw != *last_logged : *last_logged != -2) {
        int i;
        fprintf(stderr, "[collector] EGS 22 DA 2E ->");
        if (n > 0) for (i = 0; i < n; ++i) fprintf(stderr, " %02x", reply[i]);
        else fprintf(stderr, " rc %d nrc %02x", n, negative);
        fputc('\n', stderr);
        *last_logged = n >= 5 ? *raw : -2;
    }
    return 0;
}

int main(int argc, char **argv) {
    struct diag_link_cfg cfg = {0, 6801, DIAG_AUTO, DIAG_LOOP, "shiftdeck", 1};
    struct diag_link link;
    unsigned udp_port = DRIVE_PORT, cycles = 0, count = 0, gear_every = 2;
    pid_t parent = getppid();
    struct sockaddr_in dest;
    int sock, i, lost, want_speed = 1, want_gear = 0, multi = 0, gear_logged = -3;
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--host") && i + 1 < argc && !cfg.host) cfg.host = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) cfg.port = number(argv[++i], 1, 65535);
        else if (!strcmp(argv[i], "--udp-port") && i + 1 < argc) udp_port = number(argv[++i], 1, 65535);
        else if (!strcmp(argv[i], "--cycles") && i + 1 < argc) cycles = number(argv[++i], 1, 1000);
        /* ExtendedHUD (2026-11-18): the real gear from the gearbox instead of a guess from RPM and speed */
        else if (!strcmp(argv[i], "--gear")) want_gear = 1;
        else if (!strcmp(argv[i], "--gear-every") && i + 1 < argc) gear_every = number(argv[++i], 1, 20);
        else if (!strcmp(argv[i], "--no-speed")) want_speed = 0;
        else if (!strcmp(argv[i], "--multi-pid")) multi = 1;
        else if (!strcmp(argv[i], "--transport") && i + 1 < argc && !strcmp(argv[i + 1], "auto")) ++i;
        else if (!strcmp(argv[i], "--transport") && i + 1 < argc && !strcmp(argv[i + 1], "hsfz")) { cfg.transport = DIAG_HSFZ; ++i; }
        else if (!strcmp(argv[i], "--transport") && i + 1 < argc && !strcmp(argv[i + 1], "most")) { cfg.transport = DIAG_MOST; ++i; }
        else return 2;
    }
    if ((!cfg.host && cfg.transport == DIAG_HSFZ) || parent <= 1) return 2;
    diag_link_signals();
    diag_link_init(&link, &cfg);
    if (getenv("SHIFTDECK_TRACE")) most_log_level(MOST_LOG_I);   /* opt-in: the raw MOST exchange on stderr */
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return 1;
    memset(&dest, 0, sizeof dest);
    dest.sin_family = AF_INET;
    dest.sin_port = htons((uint16_t)udp_port);
    inet_pton(AF_INET, "127.0.0.1", &dest.sin_addr);
    /* One request per cycle where we can: a MOST tunnel request answers in ~260 ms (car run 5), so every value is at
       most one rotation old. With --gear every gear_every-th cycle reads the gearbox, the others the DME. */
    while (diag_running && getppid() == parent && (!cycles || count < cycles) && !diag_link_lost(&link)) {
        struct drive_frame f;
        int rpm = -1, kph = -1, gear = -1, rc = 0;
        uint64_t start = bd_mono_ms(), elapsed;
        int up = diag_link_up(&link);
        if (up) {
            if (want_gear && count % gear_every == gear_every - 1) rc = read_gear(&link, &gear, &gear_logged);
            else if (want_speed && multi) {
                rc = read_rpm_speed(&link, &rpm, &kph);
                if (!rc) {   /* the DME takes one PID per request: speed separately from now on */
                    fprintf(stderr, "[collector] 01 0C 0D: no speed in the reply, reading 01 0D alone\n");
                    multi = 0;
                    rc = read_live_pid(&link, 0x0d, 1, &kph);
                }
                if (rc == 1) rc = 0;
            } else {
                rc = read_live_pid(&link, 0x0c, 2, &rpm);
                if (!rc && want_speed) rc = read_live_pid(&link, 0x0d, 1, &kph);
            }
            if (rc == -1) diag_link_drop(&link);
            else if (!rc) {
                f.sampled_ms = bd_mono_ms();
                f.rpm = rpm;
                f.kph = kph;
                f.gear_raw = gear;
                f.mode_raw = -1;
                f.source = DRIVE_SRC_OBD;
                f.reserved = 0;
                if (sendto(sock, &f, sizeof f, 0, (struct sockaddr *)&dest, sizeof dest) != (ssize_t)sizeof f) break;
                diag_link_should_yield(&link, f.sampled_ms);
            }
        }
        diag_link_tick(&link);
        ++count;
        elapsed = bd_mono_ms() - start;
        if (diag_running) poll(0, 0, !up ? 70 : elapsed < 20 ? (int)(20 - elapsed) : 2);   /* down: retry at the old pace */
    }
    lost = diag_link_lost(&link);
    diag_link_close(&link);
    close(sock);
    return lost ? 1 : 0;
}
