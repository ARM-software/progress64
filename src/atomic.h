//Copyright (c) 2024, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _ATOMIC_H
#define _ATOMIC_H

#include <stdint.h>

#include "lockfree.h"

static inline uint32_t
atomic_load_32(const uint32_t *loc, int mm)
{
    return __atomic_load_n(loc, mm);
}

static inline uint64_t
atomic_load_64(const uint64_t *loc, int mm)
{
    return __atomic_load_n(loc, mm);
}

#define atomic_load_n(loc, mm) \
({ \
    _Generic((loc), \
	uint32_t *: atomic_load_32, \
	uint64_t *: atomic_load_64 \
	)((loc), (mm)); \
})

#define atomic_load_ptr(ptr, mm) \
((__typeof(*(ptr))) atomic_load_n((uintptr_t *)(ptr), (mm)))

static inline void
atomic_store_8(uint8_t *loc, uint8_t val, int mm)
{
    __atomic_store_n(loc, val, mm);
}

static inline void
atomic_store_32(uint32_t *loc, uint32_t val, int mm)
{
    __atomic_store_n(loc, val, mm);
}

static inline void
atomic_store_64(uint64_t *loc, uint64_t val, int mm)
{
    __atomic_store_n(loc, val, mm);
}

#define atomic_store_n(loc, val, mm) \
({ \
    _Generic((loc), \
	uint8_t *: atomic_store_8, \
	uint32_t *: atomic_store_32, \
	uint64_t *: atomic_store_64 \
	)((loc), (val), (mm)); \
})

#define atomic_store_ptr(ptr, swp, mm) \
atomic_store_n((uintptr_t *)(ptr), (uintptr_t)(swp), (mm))

static inline uint32_t
atomic_fetch_or_32(uint32_t *loc, uint32_t val, int mm)
{
    return __atomic_fetch_or(loc, val, mm);
}

static inline uint64_t
atomic_fetch_or_64(uint64_t *loc, uint64_t val, int mm)
{
    return __atomic_fetch_or(loc, val, mm);
}

#define atomic_fetch_or(loc, val, mm) \
({ \
    _Generic((loc), \
	uint32_t *: atomic_fetch_or_32, \
	uint64_t *: atomic_fetch_or_64 \
	)((loc), (val), (mm)); \
})

static inline uint32_t
atomic_exchange_32(uint32_t *loc, uint32_t swp, int mo)
{
    return __atomic_exchange_n(loc, swp, mo);
}

static inline uint64_t
atomic_exchange_64(uint64_t *loc, uint64_t swp, int mo)
{
    return __atomic_exchange_n(loc, swp, mo);
}

#define atomic_exchange_n(loc, swp, mo) \
({ \
    _Generic((loc), \
	uint32_t *: atomic_exchange_32, \
	uint64_t *: atomic_exchange_64 \
	)((loc), (swp), (mo)); \
})

#define atomic_exchange_ptr(ptr, swp, mm) \
((__typeof(*(ptr))) atomic_exchange_n((uintptr_t *)(ptr), (uintptr_t)(swp), (mm)))

static inline uint32_t
atomic_compare_exchange_32(uint32_t *loc, uint32_t *cmp, uint32_t swp, int mo_succ, int mo_fail)
{
    return __atomic_compare_exchange_n(loc, cmp, swp, false, mo_succ, mo_fail);
}

static inline uint64_t
atomic_compare_exchange_64(uint64_t *loc, uint64_t *cmp, uint64_t swp, int mo_succ, int mo_fail)
{
    return __atomic_compare_exchange_n(loc, cmp, swp, false, mo_succ, mo_fail);
}

static inline __int128
atomic_compare_exchange_128(__int128 *loc, __int128 *cmp, __int128 swp, int mo_succ, int mo_fail)
{
    return lockfree_compare_exchange_16(loc, cmp, swp, false, mo_succ, mo_fail);
}

#define atomic_compare_exchange_n(loc, cmp, swp, mo_succ, mo_fail) \
({ \
    _Generic((loc), \
	uint32_t *: atomic_compare_exchange_32, \
	uint64_t *: atomic_compare_exchange_64, \
	__int128 *: atomic_compare_exchange_128 \
	)((loc), (cmp), (swp), (mo_succ), (mo_fail)); \
})

#define atomic_compare_exchange_ptr(ptr, cmp, swp, mo_succ, mo_fail) \
((__typeof(*(ptr))) atomic_compare_exchange_n((uintptr_t *)(ptr), (uintptr_t *)cmp, (uintptr_t)(swp), (mo_succ), (mo_fail)))

#endif
