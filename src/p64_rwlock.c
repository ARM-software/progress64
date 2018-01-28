//Copyright (c) 2017, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause


#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "p64_rwlock.h"
#include "build_config.h"

#include "arch.h"

#define RWLOCK_WRITER (1U << 31)
#define RWLOCK_READERS (~RWLOCK_WRITER)

void p64_rwlock_init(p64_rwlock_t *lock)
{
    *lock = 0;
}

static inline p64_rwlock_t wait_for_no(p64_rwlock_t *lock,
				       p64_rwlock_t mask,
				       int mo)
{
    p64_rwlock_t l;
    if (((l = __atomic_load_n(lock, mo)) & mask) != 0)
    {
	SEVL();
	while (WFE() &&
	       ((l = LDXR32(lock, mo)) & mask) != 0)
	{
	    DOZE();
	}
    }
    assert((l & mask) == 0);//No threads present
    return l;
}

void p64_rwlock_acquire_rd(p64_rwlock_t *lock)
{
    p64_rwlock_t l;
    do
    {
	//Wait for any present writer to go away
	l = wait_for_no(lock, RWLOCK_WRITER, __ATOMIC_ACQUIRE);

	//Attempt to increment number of readers
    }
    while (!__atomic_compare_exchange_n(lock, &l, l + 1,
					/*weak=*/true,
					__ATOMIC_RELAXED, __ATOMIC_RELAXED));
}

void p64_rwlock_release_rd(p64_rwlock_t *lock)
{
    p64_rwlock_t prevl;
    SMP_RMB();//Load-only barrier due to reader-lock
    //Decrement number of readers
    prevl = __atomic_fetch_sub(lock, 1, __ATOMIC_RELAXED);
    //Check after lock is released but use pre-release lock value
    assert((prevl & RWLOCK_WRITER) == 0 && prevl != 0);
    (void)prevl;
}

void p64_rwlock_acquire_wr(p64_rwlock_t *lock)
{
    p64_rwlock_t l;
    do
    {
	//Wait for any present writer to go away
	l = wait_for_no(lock, RWLOCK_WRITER, __ATOMIC_ACQUIRE);

	//Attempt to set writer flag
    }
    while (!__atomic_compare_exchange_n(lock, &l, l | RWLOCK_WRITER,
					/*weak=*/true,
					__ATOMIC_RELAXED, __ATOMIC_RELAXED));

    //Wait for any present readers to go away
    (void)wait_for_no(lock, RWLOCK_READERS, __ATOMIC_RELAXED);
}

void p64_rwlock_release_wr(p64_rwlock_t *lock)
{
    assert(*lock == RWLOCK_WRITER);
    //Clear writer flag
#ifdef USE_DMB
    SMP_MB();//Load/store barrier due to writer-lock
    __atomic_store_n(lock, 0, __ATOMIC_RELAXED);
#else
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
#endif
}
