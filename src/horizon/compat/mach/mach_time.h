#pragma once
#include <stdint.h>
#include <time.h>
#include "horizon/linux_compat.h"

typedef struct { uint32_t numer, denom; } mach_timebase_info_data_t;

static inline uint64_t mach_absolute_time(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC); }
static inline int mach_timebase_info(mach_timebase_info_data_t *tb) { tb->numer = 1; tb->denom = 1; return 0; }
