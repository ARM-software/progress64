//Copyright (c) 2018-2024, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _AARCH64_H
#define _AARCH64_H

#include <stdint.h>
#include <stdlib.h>

#if defined USE_WFE
#include "ldxstx.h"
#endif

#ifdef __ARM_FEATURE_ATOMICS
//Identity-CAS - atomic read before write (CAS)
static inline uint32_t
icas4(uint32_t *ptr, int mo)
{
    uint32_t old = 0;
    if (mo == __ATOMIC_RELAXED)
    {
	__asm __volatile("cas %0, %0, [%1]"
		: "+r" (old)
		: "r" (ptr)
		: "memory");
    }
    else if (mo == __ATOMIC_ACQUIRE)
    {
	__asm __volatile("casa %0, %0, [%1]"
		: "+r" (old)
		: "r" (ptr)
		: "memory");
    }
    else
    {
	abort();
    }
    return old;
}

static inline uint64_t
icas8(uint64_t *ptr, int mo)
{
    uint64_t old = 0;
    if (mo == __ATOMIC_RELAXED)
    {
	__asm __volatile("cas %0, %0, [%1]"
		: "+r" (old)
		: "r" (ptr)
		: "memory");
    }
    else if (mo == __ATOMIC_ACQUIRE)
    {
	__asm __volatile("casa %0, %0, [%1]"
		: "+r" (old)
		: "r" (ptr)
		: "memory");
    }
    else
    {
	abort();
    }
    return old;
}

//Identity-CAS - atomic 128-bit read before write (CAS)
static inline __int128
icas16(__int128 *ptr, int mo)
{
    __int128 old = 0;
    if (mo == __ATOMIC_RELAXED)
    {
	__asm __volatile("casp %0, %H0, %0, %H0, [%1]"
		: "+r" (old)
		: "r" (ptr)
		: "memory");
    }
    else if (mo == __ATOMIC_ACQUIRE)
    {
	__asm __volatile("caspa %0, %H0, %0, %H0, [%1]"
		: "+r" (old)
		: "r" (ptr)
		: "memory");
    }
    else
    {
	abort();
    }
    return old;
}

static inline __int128
cas16(__int128 *ptr, __int128 cmp, __int128 swp, int mo)
{
    if (mo == __ATOMIC_RELAXED)
    {
	__asm __volatile("casp %0, %H0, %1, %H1, [%2]"
		: "+r" (cmp)
		: "r" (swp), "r" (ptr)
		: "memory");
    }
    else if (mo == __ATOMIC_ACQUIRE)
    {
	__asm __volatile("caspa %0, %H0, %1, %H1, [%2]"
		: "+r" (cmp)
		: "r" (swp), "r" (ptr)
		: "memory");
    }
    else
    {
	abort();
    }
    return cmp;
}
#endif

static inline void *
addr_dep(const void *ptr, uintptr_t dep)
{
    void *res;
    __asm__ volatile("eor %x0, %x1, %x2\n"
		     "eor %x0, %x0, %x2\n"
		     : "=&r" (res)
		     : "r" (ptr), "r" (dep)
		     : );
    return res;
}

static inline uint64_t
counter_freq(void)
{
    uint64_t freq;
    __asm__ volatile("mrs %0,cntfrq_el0" : "=r" (freq));
    return freq;
}

static inline uint64_t
counter_read(void)
{
    uint64_t cnt;
    __asm__ volatile("mrs %0,cntvct_el0" : "=r" (cnt));
    return cnt;
}

static inline void
wfe(void)
{
    __asm__ volatile("wfe" : : : "memory");
}

static inline void
doze(void)
{
    //Each ISB takes ~30 cycles
    __asm__ volatile("isb" : : : );
    __asm__ volatile("isb" : : : );
}

static inline void
nano_delay(uint64_t delay_ns)
{
    //Prevent speculation of subsequent counter/timer reads
    __asm __volatile("isb");
    //With these simplifications and adjustments, we are usually within 2% of the correct value
    //uint64_t delay_ticks = delay_ns * counter_freq() / 1000000000UL;
    uint64_t delay_ticks = (delay_ns + delay_ns / 16) * counter_freq() >> 30;
    if (delay_ticks != 0)
    {
	uint64_t start = counter_read();
	do
	{
	    __asm __volatile("isb" ::: "memory");
	}
	while (counter_read() - start < delay_ticks);//Handle counter roll-over
    }
    //Prevent speculation of subsequent memory accesses
    __asm __volatile("isb" ::: "memory");
}

static inline void
smp_fence(unsigned int mask)
{
    switch (mask)
    {
	case 0 :
	    break;
	case LoadLoad :
	case LoadStore :
	case LoadLoad | LoadStore :
	    __asm__ volatile ("dmb ishld" ::: "memory");
	    break;
	case StoreStore :
	    __asm__ volatile ("dmb ishst" ::: "memory");
	    break;
	case StoreLoad :
	default :
	    __asm__ volatile ("dmb ish" ::: "memory");
	    break;
    }
}

#if defined USE_WFE

#define WFE() wfe()
#define LDX(a, b) ldx((a), (b))
#define LDXPTR(a, b) (__typeof(*a))ldx((uintptr_t *)(a), (b))

#else

#define WFE() doze()
#define LDX(a, b)  __atomic_load_n((a), (b))
#define LDXPTR(a, b)  __atomic_load_n((a), (b))

#endif

static inline void
wait_until_equal8(uint8_t *loc, uint8_t val, int mm)
{
    if (__atomic_load_n(loc, mm) != val)
    {
	while(LDX(loc, mm) != val)
	{
	    WFE();
	}
    }
}

static inline void
wait_until_equal16(uint16_t *loc, uint16_t val, int mm)
{
    if (__atomic_load_n(loc, mm) != val)
    {
	while(LDX(loc, mm) != val)
	{
	    WFE();
	}
    }
}

static inline void
wait_until_equal32(uint32_t *loc, uint32_t val, int mm)
{
    if (__atomic_load_n(loc, mm) != val)
    {
	while(LDX(loc, mm) != val)
	{
	    WFE();
	}
    }
}

static inline void
wait_until_equal64(uint64_t *loc, uint64_t val, int mm)
{
    if (__atomic_load_n(loc, mm) != val)
    {
	while(LDX(loc, mm) != val)
	{
	    WFE();
	}
    }
}

static inline uint64_t
wait_until_not_equal64(uint64_t *loc, uint64_t val, int mm)
{
    uint64_t mem;
    if ((mem = __atomic_load_n(loc, mm)) == val)
    {
	while ((mem = LDX(loc, mm)) == val)
	{
	    WFE();
	}
    }
    return mem;
}

#endif
