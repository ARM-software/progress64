//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _LOCKFREE_H
#define _LOCKFREE_H

#if defined __aarch64__

#include "lockfree/aarch64.h"

#elif defined __x86_64__

#include "lockfree/x86-64.h"

#else

#error Unsupported architecture

#endif

#ifndef _ATOMIC_UMAX_4_DEFINED
#define _ATOMIC_UMAX_4_DEFINED
ALWAYS_INLINE
static inline uint32_t
lockfree_fetch_umax_4(uint32_t *var, uint32_t val, int mo_load, int mo_store)
{
    uint32_t old = __atomic_load_n(var, mo_load);
    do
    {
	if (val <= old)
	{
	    return old;
	}
    }
    while (!__atomic_compare_exchange_n(var,
					&old,
					val,
					/*weak=*/true,
					mo_store,
					__ATOMIC_RELAXED));
    return old;
}
#endif

#ifndef _ATOMIC_UMAX_8_DEFINED
#define _ATOMIC_UMAX_8_DEFINED
ALWAYS_INLINE
static inline uint64_t
lockfree_fetch_umax_8(uint64_t *var, uint64_t val, int mo_load, int mo_store)
{
    uint64_t old = __atomic_load_n(var, mo_load);
    do
    {
	if (val <= old)
	{
	    return old;
	}
    }
    while (!__atomic_compare_exchange_n(var,
					&old,
					val,
					/*weak=*/true,
					mo_store,
					__ATOMIC_RELAXED));
    return old;
}
#endif

#endif
