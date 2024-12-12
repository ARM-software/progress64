//Copyright (c) 2018-2024, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _ARCH_H
#define _ARCH_H

#include <stdint.h>

//Parameters for smp_fence()
#define LoadLoad   0x11
#define LoadStore  0x12
#define StoreLoad  0x21
#define StoreStore 0x22

static void smp_fence(unsigned int mask);

//An address dependency can be used to prevent speculative memory accesses
//A speculative memory access may bring in the cache line in shared state
//Unnecessary shared copies of cache lines that are going to be updated are
//detrimental to multithreaded scalability
static void *addr_dep(const void *ptr, uintptr_t dep);

#if defined __aarch64__

#include "arch/aarch64.h"

#elif defined __arm__

#include "arch/armv7a.h"

#elif defined __x86_64__

#include "arch/x86-64.h"

#else

#error Unsupported architecture

#endif

#define addr_dep(ptr, dep) \
((__typeof(ptr)) addr_dep((const void *)(ptr), (uintptr_t)(dep)))

#endif
