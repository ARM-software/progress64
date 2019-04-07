//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "p64_tfrwlock.h"
#include "build_config.h"

#include "arch.h"
#include "common.h"

#define RD_ONE    (1U << 16)
#define WR_ONE    1
#define WR_MASK   0xFFFF
#define TO_WR(x)  ((x) & WR_MASK)

void
p64_tfrwlock_init(p64_tfrwlock_t *lock)
{
    lock->enter.wr = 0;
    lock->enter.rd = 0;
    lock->leave.wr = 0;
    lock->leave.rd = 0;
}

static inline void
wait_until_equal16(uint16_t *loc, uint32_t val)
{
    if (__atomic_load_n(loc, __ATOMIC_ACQUIRE) != val)
    {
	SEVL();
	while(WFE() && LDXR16(loc, __ATOMIC_ACQUIRE) != val)
	{
	    DOZE();
	}
    }
}

static inline void
wait_until_equal32(uint32_t *loc, uint32_t val)
{
    if (__atomic_load_n(loc, __ATOMIC_ACQUIRE) != val)
    {
	SEVL();
	while(WFE() && LDXR32(loc, __ATOMIC_ACQUIRE) != val)
	{
	    DOZE();
	}
    }
}

void
p64_tfrwlock_acquire_rd(p64_tfrwlock_t *lock)
{
    //Increment lock->enter.rd to record reader enters
    uint32_t old_enter = __atomic_fetch_add(&lock->enter.rdwr, RD_ONE,
					    __ATOMIC_RELAXED);
    //Wait for any previous writers lockers to go away
    wait_until_equal16(&lock->leave.wr, TO_WR(old_enter));
}

void
p64_tfrwlock_release_rd(p64_tfrwlock_t *lock)
{
    //Increment lock->leave.rd to record reader leaves
    (void)__atomic_fetch_add(&lock->leave.rd, 1, __ATOMIC_RELEASE);
}

static inline uint32_t
add_w_mask(uint32_t x, uint32_t y, uint32_t mask)
{
    return ((x + y) & mask) | (x & ~mask);
}

static inline uint32_t
atomic_add_w_mask(uint32_t *loc, uint32_t val, uint32_t mask)
{
    uint32_t old, neu;
#ifndef USE_LDXSTX
    old = __atomic_load_n(loc, __ATOMIC_RELAXED);
#endif
    do
    {
#ifdef USE_LDXSTX
	old = ldx32(loc, __ATOMIC_RELAXED);
#endif
	neu = add_w_mask(old, val, mask);
    }
#ifdef USE_LDXSTX
    while (UNLIKELY(stx32(loc, neu, __ATOMIC_RELAXED)));
#else
    while (!__atomic_compare_exchange_n(loc,
					&old,//Updated on failure
					neu,
					/*weak=*/true,
					__ATOMIC_RELAXED,
					__ATOMIC_RELAXED));
#endif
    return old;
}

void
p64_tfrwlock_acquire_wr(p64_tfrwlock_t *lock)
{
    //Increment lock->enter.wr to acquire a writer ticket
    uint32_t old_enter = atomic_add_w_mask(&lock->enter.rdwr, WR_ONE, WR_MASK);
    //Wait for my turn among writers and wait for previously present readers
    //to leave
    //New writers may arrive (will increment lock->enter.wr)
    //New readers may arrive (will increment lock->enter.rd) but these
    //will wait for me to leave and increment lock->leave.wr
    wait_until_equal32(&lock->leave.rdwr, old_enter);
}

void
p64_tfrwlock_release_wr(p64_tfrwlock_t *lock)
{
    //Increment lock->leave.wr to release writer ticket
    uint32_t my_tkt = __atomic_load_n(&lock->leave.wr, __ATOMIC_RELAXED);
    (void)__atomic_store_n(&lock->leave.wr, my_tkt + 1, __ATOMIC_RELEASE);
}
