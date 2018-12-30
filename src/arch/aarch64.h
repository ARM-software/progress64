//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _AARCH64_H
#define _AARCH64_H

#include <stdint.h>
#include <stdlib.h>

static inline void sevl(void)
{
    __asm__ volatile("sevl" : : : );
}

static inline int wfe(void)
{
    __asm__ volatile("wfe" : : : "memory");
    return 1;
}

static inline void doze(void)
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

#if defined USE_WFE

#define SEVL() sevl()
#define WFE() wfe()
#define LDXR8(a, b)  ldx8(a, b)
#define LDXR16(a, b) ldx16(a, b)
#define LDXR32(a, b) ldx32(a, b)
#define LDXR64(a, b) ldx64(a, b)
#define LDXR128(a, b) ldx128(a, b)
//When using WFE do not stall the pipeline using other means (e.g. NOP or ISB)
#define DOZE() (void)0

#include "ldxstx.h"

#else

#define SEVL() (void)0
#define WFE() 1
#define LDXR8(a, b)  __atomic_load_n(a, b)
#define LDXR16(a, b) __atomic_load_n(a, b)
#define LDXR32(a, b) __atomic_load_n(a, b)
#define LDXR64(a, b) __atomic_load_n(a, b)
#define LDXR128(a, b) __atomic_load_n(a, b)
#define DOZE() doze()

#endif

#endif
