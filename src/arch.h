//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _ARCH_H
#define _ARCH_H

//Parameters for smp_fence()
#define LoadLoad   0x11
#define LoadStore  0x12
#define StoreLoad  0x21
#define StoreStore 0x22

static void smp_fence(unsigned int mask);

#define wait_until_equal(loc, val, mm) \
_Generic((loc), \
    uint8_t *: wait_until_equal8, \
    uint16_t *: wait_until_equal16, \
    uint32_t *: wait_until_equal32 \
    )((loc), (val), (mm))

#if defined __aarch64__

#include "arch/aarch64.h"

#elif defined __arm__

#include "arch/armv7a.h"

#elif defined __x86_64__

#include "arch/x86-64.h"

#else

#error Unsupported architecture

#endif

#endif
