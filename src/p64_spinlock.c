//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdbool.h>

#include "p64_spinlock.h"
#include "build_config.h"

#include "arch.h"

void p64_spin_init(p64_spinlock_t *lock)
{
    *lock = 0;
}

static inline int try_lock(p64_spinlock_t *lock)
{
    p64_spinlock_t old = 0;
    //Weak is normally better when using exclusives and retrying
    return __atomic_compare_exchange_n(lock, &old, 1, /*weak=*/true,
				       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

void p64_spin_lock(p64_spinlock_t *lock)
{
    do
    {
	if (__atomic_load_n(lock, __ATOMIC_RELAXED) != 0)
	{
	    SEVL();
	    while (WFE() && LDXR8(lock, __ATOMIC_RELAXED) != 0)
	    {
		DOZE();
	    }
	}
	//*lock == 0
    }
    while (!try_lock(lock));
}

void p64_spin_unlock(p64_spinlock_t *lock)
{
    //Order both loads and stores
#ifdef USE_DMB
    SMP_MB();
    __atomic_store_n(lock, 0, __ATOMIC_RELAXED);
#else
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
#endif
}

void p64_spin_unlock_ro(p64_spinlock_t *lock)
{
    //Order only loads
    SMP_RMB();
    __atomic_store_n(lock, 0, __ATOMIC_RELAXED);
}
