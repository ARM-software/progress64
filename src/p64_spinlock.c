//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdbool.h>
#include <stdio.h>

#include "p64_spinlock.h"
#include "build_config.h"

#include "common.h"
#include "arch.h"

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
    //Wait until lock is available
    do
    {
	if (__atomic_load_n(lock, __ATOMIC_RELAXED) != 0)
	{
	    SEVL();
	    while (WFE() && LDX(lock, __ATOMIC_RELAXED) != 0)
	    {
		DOZE();
	    }
	}
	//*lock == 0
    }
    while (!try_lock(lock, /*weak=*/true));
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
#ifdef USE_DMB
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(lock, 0, __ATOMIC_RELAXED);
#else
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
#endif
}

void
p64_spinlock_release_ro(p64_spinlock_t *lock)
{
    //Order only loads
    smp_fence(LoadStore);
    __atomic_store_n(lock, 0, __ATOMIC_RELAXED);
}
