#ifndef DRIVE_FRAME_H
#define DRIVE_FRAME_H

#include <stdint.h>

enum { DRIVE_PORT = 42863, DRIVE_STALE_MS = 750 };

/* -1 in a field: not read in this frame; the receiver keeps its last value of that field. */
struct drive_frame {
    uint64_t sampled_ms;
    int32_t rpm;
    int32_t kph;
    int32_t gear_raw;    /* the source's raw gear/selector value (hud_gear.h decodes it), -1 not read */
    int32_t mode_raw;    /* the source's raw driving mode (Comfort/Sport/...; hud_gear.h), -1 not read */
    int32_t source;      /* DRIVE_SRC_* */
    int32_t reserved;    /* 0; keeps the size 32 on every ABI (no tail padding) */
};

enum { DRIVE_SRC_OBD = 0, DRIVE_SRC_CDS = 1 };

#endif
