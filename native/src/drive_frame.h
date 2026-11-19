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

/* The car's position, from the plugin's CDS feed (navigation.GPSPosition / GPSExtendedInfo), on the same port. 40 bytes, so
   a receiver tells it from a drive_frame by its size, then by the magic. */
enum { GEO_MAGIC = 0x47454F31, GEO_STALE_MS = 3000 };   /* "GEO1" */
struct geo_frame {
    uint32_t magic;
    int32_t lat_e6, lon_e6;   /* WGS84 degrees x 1e6 */
    int32_t heading;          /* degrees 0..359 from north, -1 not known (the worker takes it from the track) */
    uint64_t sampled_ms;
    int32_t kph;              /* -1 not known */
    int32_t reserved[3];      /* 0 */
};

#endif
