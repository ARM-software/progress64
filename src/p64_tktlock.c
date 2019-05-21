//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdint.h>

#include "p64_tktlock.h"
#include "build_config.h"

#include "arch.h"
#include "common.h"

void
p64_tktlock_init(p64_tktlock_t *lock)
{
    lock->enter = 0;
    lock->leave = 0;
}

static inline void
wait_until_equal16(uint16_t *loc, uint16_t val)
{
    if (__atomic_load_n(loc, __ATOMIC_ACQUIRE) != val)
    {
	SEVL();
	while(WFE() && LDX(loc, __ATOMIC_ACQUIRE) != val)
	{
	    DOZE();
	}
    }
}

void
p64_tktlock_acquire(p64_tktlock_t *lock, uint16_t *tkt)
{
    PREFETCH_ATOMIC(lock);
    //Get a ticket
    *tkt = __atomic_fetch_add(&lock->enter, 1, __ATOMIC_RELAXED);
    //Wait for any previous lockers to go away
    wait_until_equal16(&lock->leave, *tkt);
}

void
p64_tktlock_release(p64_tktlock_t *lock, uint16_t tkt)
{
    //Release ticket
#ifdef USE_DMB
    __atomic_thread_fence(__ATOMIC_RELEASE);
    (void)__atomic_store_n(&lock->leave, tkt + 1, __ATOMIC_RELAXED);
#else
    (void)__atomic_store_n(&lock->leave, tkt + 1, __ATOMIC_RELEASE);
#endif
}
