//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdbool.h>

#include "p64_spinlock.h"
#include "build_config.h"

#include "common.h"
#include "atomic.h"

void
p64_spinlock_init(p64_spinlock_t *lock)
{
    *lock = 0;
}

static inline int
try_lock(p64_spinlock_t *lock)
{
    p64_spinlock_t old = 0;
    //Weak is normally better when using exclusives and retrying
    return atomic_compare_exchange_n(lock, &old, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

void
p64_spinlock_acquire(p64_spinlock_t *lock)
{
    do
    {
	//Wait until lock is available
	wait_until_equal_w_bkoff(lock, 0, 60, __ATOMIC_RELAXED);
    }
    while (!try_lock(lock));
}

bool
p64_spinlock_try_acquire(p64_spinlock_t *lock)
{
    if (atomic_load_n(lock, __ATOMIC_RELAXED) == 0)
    {
	//Lock is available, try hard once to acquire it
	return try_lock(lock);
    }
    //Lock is not available, don't wait
    return false;
}

void
p64_spinlock_release(p64_spinlock_t *lock)
{
    //Order both loads and stores
    atomic_store_n(lock, 0, __ATOMIC_RELEASE);
}

void
p64_spinlock_release_ro(p64_spinlock_t *lock)
{
    //Order only loads
    smp_fence(LoadStore);
    atomic_store_n(lock, 0, __ATOMIC_RELAXED);
}
