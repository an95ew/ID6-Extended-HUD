#ifndef EXTENDEDHUD_GAUGE_CORE_H
#define EXTENDEDHUD_GAUGE_CORE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Monotonic clock in milliseconds (CLOCK_MONOTONIC), shared by processes on the same QNX node. */
uint64_t bd_mono_ms(void);
#ifdef __cplusplus
}
#endif
#endif
