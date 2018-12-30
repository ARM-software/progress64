//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _X86_64_H
#define _X86_64_H

#include <stdint.h>
#include <stdlib.h>

static inline void doze(void)
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

#define SEVL() (void)0
#define WFE() 1
#define LDXR8(a, b)  __atomic_load_n(a, b)
#define LDXR16(a, b) __atomic_load_n(a, b)
#define LDXR32(a, b) __atomic_load_n(a, b)
#define LDXR64(a, b) __atomic_load_n(a, b)
#define LDXR128(a, b) __atomic_load_n(a, b)
#define DOZE() doze()

#endif
