//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdbool.h>

#include "p64_spinlock.h"
#include "build_config.h"

#include "common.h"
#include "arch.h"
#ifdef USE_LDXSTX
#include "ldxstx.h"
#endif

void
p64_spinlock_init(p64_spinlock_t *lock)
{
    *lock = 0;
}

static inline int
try_lock(p64_spinlock_t *lock, bool weak)
{
    p64_spinlock_t old = 0;
    //Weak is normally better when using exclusives and retrying
    return __atomic_compare_exchange_n(lock, &old, 1, /*weak=*/weak,
				       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

void
p64_spinlock_acquire(p64_spinlock_t *lock)
{
#ifdef USE_LDXSTX
    SEVL();
wait_for_event:
    (void)WFE();
    do
    {
	if (ldx(lock, __ATOMIC_ACQUIRE) != 0)
	{
	    //Lock already taken, wait until updated
	    DOZE();
	    goto wait_for_event;
	}
    }
    while (UNLIKELY(stx(lock, 1, __ATOMIC_RELAXED)));
#else
    do
    {
	//Wait until lock is available
	wait_until_equal(lock, 0, __ATOMIC_RELAXED);
    }
    while (!try_lock(lock, /*weak=*/true));
#endif
}

bool
p64_spinlock_try_acquire(p64_spinlock_t *lock)
{
    if (__atomic_load_n(lock, __ATOMIC_RELAXED) == 0)
    {
	//Lock is available, try hard once to acquire it
	return try_lock(lock, /*weak=*/false);
    }
    //Lock is not available, don't wait
    return false;
}

void
p64_spinlock_release(p64_spinlock_t *lock)
{
    //Order both loads and stores
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
}

void
p64_spinlock_release_ro(p64_spinlock_t *lock)
{
    //Order only loads
    smp_fence(LoadStore);
    __atomic_store_n(lock, 0, __ATOMIC_RELAXED);
}
