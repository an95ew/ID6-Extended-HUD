#define _POSIX_C_SOURCE 200809L
#include "gauge_core.h"
#include <time.h>

uint64_t bd_mono_ms(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
