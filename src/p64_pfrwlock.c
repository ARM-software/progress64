//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdbool.h>

#include "p64_pfrwlock.h"
#include "build_config.h"

#include "atomic.h"
#include "common.h"

// uint16_t enter_rd;//Bits 0..15
// uint16_t pend_rd;//Bits 16..31
// uint16_t leave_wr;//Bits 32..47
// uint16_t enter_wr;//Bits 48..63

#define ENTER_RD(x)  (uint16_t)((x) >> ENTER_RD_SHIFT)
#define PEND_RD(x)   (uint16_t)((x) >> PEND_RD_SHIFT)
#define LEAVE_WR(x)  (uint16_t)((x) >> LEAVE_WR_SHIFT)
#define ENTER_WR(x)  (uint16_t)((x) >> ENTER_WR_SHIFT)

#define ENTER_RD_SHIFT 0
#define ENTER_RD_MASK  0xFFFFU
#define ENTER_RD_ONE   1
#define PEND_RD_SHIFT  16
#define PEND_RD_MASK   (0xFFFFU << PEND_RD_SHIFT)
#define PEND_RD_ONE    (1U << PEND_RD_SHIFT)
#define LEAVE_WR_SHIFT 32
#define ENTER_WR_SHIFT 48
#define ENTER_WR_ONE   (UINT64_C(1) << ENTER_WR_SHIFT)

void
p64_pfrwlock_init(p64_pfrwlock_t *lock)
{
    lock->enter_rd = 0;
    lock->pend_rd = 0;
    lock->leave_wr = 0;
    lock->enter_wr = 0;
    lock->leave_rd = 0;
}

static inline uint64_t
add_w_mask(uint64_t x, uint64_t y, uint64_t mask)
{
    return ((x + y) & mask) | (x & ~mask);
}

static inline uint64_t
atomic_incr_enter_or_pend(uint64_t *loc)
{
    uint64_t old, neu;
    old = atomic_load_n(loc, __ATOMIC_RELAXED);
    do
    {
	if (ENTER_WR(old) == LEAVE_WR(old))
	{
	    //No writers pending, increment enter_rd
	    neu = add_w_mask(old, ENTER_RD_ONE, ENTER_RD_MASK);
	}
	else
	{
	    //Writers pending, increment pend_rd
	    neu = add_w_mask(old, PEND_RD_ONE, PEND_RD_MASK);
	}
    }
    while (!atomic_compare_exchange_n(loc,
				      &old,//Updated on failure
				      neu,
				      __ATOMIC_ACQUIRE,
				      __ATOMIC_ACQUIRE));
    return old;
}

void
p64_pfrwlock_acquire_rd(p64_pfrwlock_t *lock)
{
    //Increment 'enter_rd' or 'pend_rd' depending on any writer present
    uint64_t old = atomic_incr_enter_or_pend(&lock->word);
    if (ENTER_WR(old) != LEAVE_WR(old))
    {
	//Writer(s) present, wait for next reader phase
	wait_until_equal(&lock->leave_wr, LEAVE_WR(old) + 1, __ATOMIC_ACQUIRE);
    }
    //Else no writers present, return immediately
}

void
p64_pfrwlock_release_rd(p64_pfrwlock_t *lock)
{
    //Increment 'leave_rd' to record reader leaves
    (void)atomic_fetch_add(&lock->leave_rd, 1, __ATOMIC_RELEASE);
}

void
p64_pfrwlock_acquire_wr(p64_pfrwlock_t *lock)
{
    //Increment 'enter_wr' to acquire a writer ticket
    uint64_t old = atomic_fetch_add(&lock->word,
				      ENTER_WR_ONE, __ATOMIC_RELAXED);
    //Wait for previous writer to end write phase and update 'enter_rd'
    wait_until_equal(&lock->leave_wr, ENTER_WR(old), __ATOMIC_ACQUIRE);
    //'enter_rd' now updated from 'pend_rd'
    uint16_t enter_rd = atomic_load_n(&lock->enter_rd, __ATOMIC_RELAXED);
    //Wait for all previous readers to leave
    wait_until_equal(&lock->leave_rd, enter_rd, __ATOMIC_RELAXED);
}

void
p64_pfrwlock_release_wr(p64_pfrwlock_t *lock)
{
    uint64_t *loc = (uint64_t *)lock;
    uint64_t old, neu;
    old = atomic_load_n(loc, __ATOMIC_RELAXED);
    do
    {
	//Compute new values
	uint16_t enter_wr = ENTER_WR(old);
	uint16_t leave_wr = LEAVE_WR(old) + 1;
	uint16_t enter_rd = ENTER_RD(old) + PEND_RD(old);
	//'pend_rd' is cleared
	neu = ((uint64_t)enter_wr << ENTER_WR_SHIFT) |
	      ((uint64_t)leave_wr << LEAVE_WR_SHIFT) |
	      ((uint64_t)enter_rd << ENTER_RD_SHIFT);
    }
    while (!atomic_compare_exchange_n(loc,
				      &old,//Updated on failure
				      neu,
				      __ATOMIC_RELEASE,
				      __ATOMIC_RELAXED));
}
