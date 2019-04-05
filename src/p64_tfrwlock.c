//Copyright (c) 2017, ARM Limited. All rights reserved.
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

#define RD_ENTER_ONE   (1UL << 16)
#define WR_TICKET_ONE  1
#define WR_TICKET_MASK 0xFFFF

#define WR_TICKET(x)  ((uint16_t) (x)       )
#define RD_ENTER(x)   ((uint16_t)((x) >> 16))

void
p64_tfrwlock_init(p64_tfrwlock_t *lock)
{
    lock->wr_ticket = 0;
    lock->rd_enter = 0;
    lock->wr_serving = 0;
    lock->rd_leave = 0;
}

static inline void
wait_until_equal(uint16_t *loc, uint32_t val)
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

void
p64_tfrwlock_acquire_rd(p64_tfrwlock_t *lock)
{
    //Increment lock->rd_enter to signal shared locker enters
    uint32_t old_word = __atomic_fetch_add(&lock->word, RD_ENTER_ONE,
					   __ATOMIC_RELAXED);
    uint16_t wr_ticket = WR_TICKET(old_word);
    //Wait for any previous exclusive lockers to go away
    wait_until_equal(&lock->wr_serving, wr_ticket);
}

void
p64_tfrwlock_release_rd(p64_tfrwlock_t *lock)
{
    //Increment lock->rd_leave to signal shared locker leaves
    (void)__atomic_fetch_add(&lock->rd_leave, 1, __ATOMIC_RELEASE);
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
    //Increment lock->wr_ticket to acquire an exclusive ticket
    uint32_t old_word = atomic_add_w_mask(&lock->word, WR_TICKET_ONE, WR_TICKET_MASK);
    uint16_t my_tkt = WR_TICKET(old_word);//Pre-increment value
    uint16_t rd_enter = RD_ENTER(old_word);
    //Wait for my turn among exclusive lockers
    //New exclusive lockers may arrive (will increment lock->wr_ticket)
    //New shared lockers may arrive (will increment lock->rd_enter)
    wait_until_equal(&lock->wr_serving, my_tkt);
    //Wait for previously present shared lockers to go away
    wait_until_equal(&lock->rd_leave, rd_enter);
}

void
p64_tfrwlock_release_wr(p64_tfrwlock_t *lock)
{
    //Increment lock->wr_serving to release exclusive ticket
    uint32_t my_tkt = __atomic_load_n(&lock->wr_serving, __ATOMIC_RELAXED);
    (void)__atomic_store_n(&lock->wr_serving, my_tkt + 1, __ATOMIC_RELEASE);
}
