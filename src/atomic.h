//Copyright (c) 2024, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _ATOMIC_H
#define _ATOMIC_H

#include <stdint.h>

#include "lockfree.h"

#define atomic_load_n(loc, mm) \
({ \
    _Generic((loc), \
	uint8_t  *: __atomic_load_n, \
	uint16_t *: __atomic_load_n, \
	uint32_t *: __atomic_load_n, \
	uint64_t *: __atomic_load_n \
	)((loc), (mm)); \
})

#define atomic_load_ptr(ptr, mm) \
((__typeof(*(ptr))) atomic_load_n((uintptr_t *)(ptr), (mm)))

#define atomic_store_n(loc, val, mm) \
({ \
    _Generic((loc), \
	uint8_t  *: __atomic_store_n, \
	uint16_t *: __atomic_store_n, \
	uint32_t *: __atomic_store_n, \
	uint64_t *: __atomic_store_n \
	)((loc), (val), (mm)); \
})

#define atomic_store_ptr(ptr, swp, mm) \
atomic_store_n((uintptr_t *)(ptr), (uintptr_t)(swp), (mm))

#define atomic_fetch_or(loc, val, mm) \
({ \
    _Generic((loc), \
	uint32_t *: __atomic_fetch_or, \
	uint64_t *: __atomic_fetch_or \
	)((loc), (val), (mm)); \
})

#define atomic_exchange_n(loc, swp, mo) \
({ \
    _Generic((loc), \
	uint32_t *: __atomic_exchange_n, \
	uint64_t *: __atomic_exchange_n \
	)((loc), (swp), (mo)); \
})

#define atomic_exchange_ptr(ptr, swp, mm) \
((__typeof(*(ptr))) atomic_exchange_n((uintptr_t *)(ptr), (uintptr_t)(swp), (mm)))

#define atomic_compare_exchange_n(loc, cmp, swp, mo_succ, mo_fail) \
({ \
    _Generic((loc), \
	uint32_t *: __atomic_compare_exchange_n, \
	uint64_t *: __atomic_compare_exchange_n, \
	__int128 *: lockfree_compare_exchange_16 \
	)((loc), (cmp), (swp), false, (mo_succ), (mo_fail)); \
})

#define atomic_compare_exchange_ptr(ptr, cmp, swp, mo_succ, mo_fail) \
((__typeof(*(ptr))) atomic_compare_exchange_n((uintptr_t *)(ptr), (uintptr_t *)cmp, (uintptr_t)(swp), (mo_succ), (mo_fail)))

#endif
