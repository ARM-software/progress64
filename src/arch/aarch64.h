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

#define addr_dep(ptr, dep) \
((__typeof(ptr)) addr_dep((const void *)(ptr), (uintptr_t)(dep)))

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

static inline uint32_t
wait_until_equal2_32(uint32_t *loc, uint32_t val0, uint32_t val1, int mm)
{
    uint32_t v;
    if ((v = __atomic_load_n(loc, mm)) != val0 && v != val1)
    {
	while((v = LDX(loc, mm)) != val0 && v != val1)
	{
	    WFE();
	}
    }
    return v;
}

#endif
