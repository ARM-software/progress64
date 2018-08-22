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

//Full fence, e.g. for store/load ordering
#define SMP_MB()   __asm__ volatile("mfence" : : : "memory");
//Read fence for load/load ordering - no-op on x86-64
#define SMP_RMB()  __asm__ volatile(""       : : : "memory");
//Write fence for store/store ordering - no-op on x86-64
#define SMP_WMB()  __asm__ volatile(""       : : : "memory");

#define SEVL() (void)0
#define WFE() 1
#define LDXR8(a, b)  __atomic_load_n(a, b)
#define LDXR16(a, b) __atomic_load_n(a, b)
#define LDXR32(a, b) __atomic_load_n(a, b)
#define LDXR64(a, b) __atomic_load_n(a, b)
#define LDXR128(a, b) __atomic_load_n(a, b)
#define DOZE() doze()

#endif
