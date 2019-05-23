//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _X86_64_H
#define _X86_64_H

#include <stdint.h>
#include <stdlib.h>

static inline uint64_t
counter_freq(void)
{
    return 3000000000;//FIXME
}

static inline uint64_t
counter_read(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a" (lo), "=d" (hi));
    return (uint64_t)hi << 32 | lo;
}

static inline void
doze(void)
{
    __asm__ volatile("rep; nop" : : : );
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

static inline void
wait_until_equal8(uint8_t *loc, uint8_t val, int mm)
{
    while (__atomic_load_n(loc, mm) != val)
    {
	doze();
    }
}

static inline void
wait_until_equal16(uint16_t *loc, uint16_t val, int mm)
{
    while (__atomic_load_n(loc, mm) != val)
    {
	doze();
    }
}

static inline void
wait_until_equal32(uint32_t *loc, uint32_t val, int mm)
{
    while (__atomic_load_n(loc, mm) != val)
    {
	doze();
    }
}

#define SEVL() (void)0
#define WFE() 1
#define LDX(a, b)  __atomic_load_n((a), (b))
#define LDXPTR(a, b)  __atomic_load_n((a), (b))
#define DOZE() doze()

#endif
