//Copyright (c) 2024, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _INLINE_H
#define _INLINE_H

#include "arch.h"

#define wait_until_equal(loc, val, mm) \
({ \
    _Generic((loc), \
	uint8_t *: wait_until_equal8, \
	uint16_t *: wait_until_equal16, \
	uint32_t *: wait_until_equal32, \
	uint64_t *: wait_until_equal64 \
	)((loc), (val), (mm)); \
})

#define wait_until_not_equal(loc, val, mm) \
({ \
    _Generic((loc), \
	uint64_t *: wait_until_not_equal64 \
	)((loc), (val), (mm)); \
})

#define wait_until_not_equal_ptr(ptr, val, mm) \
((__typeof(*(ptr))) wait_until_not_equal((uintptr_t *)(ptr), (uintptr_t)(val), (mm)))

static inline void
wait_until_equal_w_bkoff8(uint8_t *loc, uint8_t val, uint32_t dly, int mm)
{
    while (__atomic_load_n(loc, mm) != val)
    {
	nano_delay(dly);
    }
}

static inline void
wait_until_equal_w_bkoff16(uint16_t *loc, uint16_t val, uint32_t dly, int mm)
{
    while (__atomic_load_n(loc, mm) != val)
    {
	nano_delay(dly);
    }
}

static inline void
wait_until_equal_w_bkoff32(uint32_t *loc, uint32_t val, uint32_t dly, int mm)
{
    while (__atomic_load_n(loc, mm) != val)
    {
	nano_delay(dly);
    }
}

static inline void
wait_until_equal_w_bkoff64(uint64_t *loc, uint64_t val, uint32_t dly, int mm)
{
    while (__atomic_load_n(loc, mm) != val)
    {
	nano_delay(dly);
    }
}

#define wait_until_equal_w_bkoff(loc, val, dly, mm) \
({ \
    _Generic((loc), \
	uint8_t *: wait_until_equal_w_bkoff8, \
	uint16_t *: wait_until_equal_w_bkoff16, \
	uint32_t *: wait_until_equal_w_bkoff32, \
	uint64_t *: wait_until_equal_w_bkoff64 \
	)((loc), (val), (dly), (mm)); \
})

#endif
