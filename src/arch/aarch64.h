//Copyright (c) 2018, ARM Limited. All rights reserved.
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
sevl(void)
{
    __asm__ volatile("sevl" : : : );
}

static inline int
wfe(void)
{
    __asm__ volatile("wfe" : : : "memory");
    return 1;
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

static inline void
wait_until_equal8(uint8_t *loc, uint8_t val, int mm)
{
#ifdef USE_WFE
    if (__atomic_load_n(loc, mm) != val)
    {
	sevl();
	while(wfe() && ldx(loc, mm) != val)
	{
	}
    }
#else
    while (__atomic_load_n(loc, mm) != val)
    {
	doze();
    }
#endif
}

static inline void
wait_until_equal16(uint16_t *loc, uint16_t val, int mm)
{
#ifdef USE_WFE
    if (__atomic_load_n(loc, mm) != val)
    {
	sevl();
	while(wfe() && ldx(loc, mm) != val)
	{
	}
    }
#else
    while (__atomic_load_n(loc, mm) != val)
    {
	doze();
    }
#endif
}

static inline void
wait_until_equal32(uint32_t *loc, uint32_t val, int mm)
{
#ifdef USE_WFE
    if (__atomic_load_n(loc, mm) != val)
    {
	sevl();
	while(wfe() && ldx(loc, mm) != val)
	{
	}
    }
#else
    while (__atomic_load_n(loc, mm) != val)
    {
	doze();
    }
#endif
}

static inline void
wait_until_equal64(uint64_t *loc, uint64_t val, int mm)
{
#ifdef USE_WFE
    if (__atomic_load_n(loc, mm) != val)
    {
	sevl();
	while(wfe() && ldx(loc, mm) != val)
	{
	}
    }
#else
    while (__atomic_load_n(loc, mm) != val)
    {
	doze();
    }
#endif
}

static inline uint32_t
wait_until_equal2_32(uint32_t *loc, uint32_t val0, uint32_t val1, int mm)
{
    uint32_t v;
#ifdef USE_WFE
    if ((v = __atomic_load_n(loc, mm)) != val0 && v != val1)
    {
	sevl();
	while(wfe() && (v = ldx(loc, mm)) != val0 && v != val1)
	{
	}
    }
#else
    while ((v = __atomic_load_n(loc, mm)) != val0 && v != val1)
    {
	doze();
    }
#endif
    return v;
}

#if defined USE_WFE

#define SEVL() sevl()
#define WFE() wfe()
#define LDX(a, b) ldx((a), (b))
#define LDXPTR(a, b) (__typeof(*a))ldx((uintptr_t *)(a), (b))
//When using WFE do not stall the pipeline using other means (e.g. NOP or ISB)
#define DOZE() (void)0

#else

#define SEVL() (void)0
#define WFE() 1
#define LDX(a, b)  __atomic_load_n((a), (b))
#define LDXPTR(a, b)  __atomic_load_n((a), (b))
#define DOZE() doze()

#endif

#endif
