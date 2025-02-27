//Copyright (c) 2018-2024, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _X86_64_H
#define _X86_64_H

#include <stdint.h>
#include <stdlib.h>
//A hack to avoid having to define _POSIX_C_SOURCE on the command line
#define __USE_POSIX199309 1
#include <time.h>

static inline void *
addr_dep(const void *ptr, uintptr_t dep)
{
    void *res;
    __asm ("movq %1,%0\n"
	   "xor %2, %0\n"
	   "xor %2, %0\n"
	   : "=&r" (res)
	   : "r" (ptr), "r" (dep)
	   : );
    return res;
}

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
#ifndef VERIFY
    __asm__ volatile("pause" : : : "memory");
#endif
}

static inline void
nano_delay(uint64_t delay_ns)
{
    while (delay_ns >= 50)
    {
	__asm__ volatile("pause" : : : "memory");
	//Assume each PAUSE instruction takes 50ns (e.g. 150 cycles @ 3GHz)
	delay_ns -= 50;
    }
    //TODO insert speculation barrier here
}

static inline void
smp_fence(unsigned int mask)
{
    if ((mask & StoreLoad) == StoreLoad)
    {
	__asm__ volatile ("mfence" ::: "memory");
    }
    else if (mask != 0)
    {
	//Any fence but StoreLoad
	__asm__ volatile ("" ::: "memory");
    }
    //Else no fence specified
}

#define WFE() doze()
#define LDX(a,b) __atomic_load_n((a), (b))
#define LDXPTR(a,b) __atomic_load_n((a), (b))

#endif
