//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
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

void
p64_tfrwlock_acquire_rd(p64_tfrwlock_t *lock)
{
    //Increment lock->enter.rd to record reader enters
    uint32_t old_enter = __atomic_fetch_add(&lock->enter.rdwr, RD_ONE,
					    __ATOMIC_RELAXED);
    //Wait for any previous writers lockers to go away
    wait_until_equal(&lock->leave.wr, TO_WR(old_enter), __ATOMIC_ACQUIRE);
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
    old = __atomic_load_n(loc, __ATOMIC_RELAXED);
    do
    {
	neu = add_w_mask(old, val, mask);
    }
    while (!__atomic_compare_exchange_n(loc,
					&old,//Updated on failure
					neu,
					/*weak=*/true,
					__ATOMIC_RELAXED,
					__ATOMIC_RELAXED));
    return old;
}

void
p64_tfrwlock_acquire_wr(p64_tfrwlock_t *lock, uint16_t *tkt)
{
    //Increment lock->enter.wr to acquire a writer ticket
    uint32_t old_enter = atomic_add_w_mask(&lock->enter.rdwr, WR_ONE, WR_MASK);
    *tkt = TO_WR(old_enter);
    //Wait for my turn among writers and wait for previously present readers
    //to leave
    //New writers may arrive (will increment lock->enter.wr)
    //New readers may arrive (will increment lock->enter.rd) but these
    //will wait for me to leave and increment lock->leave.wr
    wait_until_equal(&lock->leave.rdwr, old_enter, __ATOMIC_ACQUIRE);
}

void
p64_tfrwlock_release_wr(p64_tfrwlock_t *lock, uint16_t tkt)
{
    //Increment lock->leave.wr to release writer ticket
    (void)__atomic_store_n(&lock->leave.wr, tkt + 1, __ATOMIC_RELEASE);
}
