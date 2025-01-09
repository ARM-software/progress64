//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _ARMV7A_H
#define _ARMV7A_H

#include <stdint.h>
#include <stdlib.h>
//A hack to avoid having to define _POSIX_C_SOURCE on the command line
#define __USE_POSIX199309 1
#include <time.h>

#if defined USE_WFE
#error
#endif

static inline uint64_t
counter_freq(void)
{
    return UINT64_C(1000000000);
}

static inline uint64_t
counter_read(void)
{
    struct timespec ts;
    while (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0)
    {
    }
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + ts.tv_nsec;
}

static inline void
doze(void)
{
    __asm__ volatile("isb" : : : );//isb better than nop
}

static inline void
smp_fence(unsigned int mask)
{
    switch (mask)
    {
	case 0 :
	    break;
	case StoreLoad :
	case StoreStore :
	    //DMB ST only orders stores before the barrier
	    __asm__ volatile ("dmb ishst" ::: "memory");
	    break;
	case LoadLoad :
	case LoadStore :
	case LoadLoad | LoadStore :
	    //To order loads before the barrier we need full DMB
	default :
	    __asm__ volatile ("dmb ish" ::: "memory");
	    break;
    }
}

#define WFE() doze()
#define LDX(a, b)  __atomic_load_n((a), (b))
#define LDXPTR(a, b)  __atomic_load_n((a), (b))

#endif
