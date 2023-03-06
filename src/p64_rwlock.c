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
#include "common.h"
#include "err_hnd.h"

#define RWLOCK_WRITER (1U << 31)
#define RWLOCK_READERS (~RWLOCK_WRITER)

void
p64_rwlock_init(p64_rwlock_t *lock)
{
    *lock = 0;
}

static inline
p64_rwlock_t wait_for_no(p64_rwlock_t *lock,
			 p64_rwlock_t mask,
			 int mo)
{
    p64_rwlock_t l;
    if (((l = __atomic_load_n(lock, mo)) & mask) != 0)
    {
	SEVL();
	while (WFE() &&
	       ((l = LDX(lock, mo)) & mask) != 0)
	{
	    DOZE();
	}
    }
    assert((l & mask) == 0);//No threads present
    return l;
}

void
p64_rwlock_acquire_rd(p64_rwlock_t *lock)
{
    p64_rwlock_t l;
    do
    {
	//Wait for any present writer to go away
	l = wait_for_no(lock, RWLOCK_WRITER, __ATOMIC_RELAXED);
	assert((l & RWLOCK_WRITER) == 0);

	//Attempt to increment number of readers
    }
    //A0: read lock.w, synchronize with A3
    while (!__atomic_compare_exchange_n(lock, &l, l + 1,
					/*weak=*/true,
					__ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE));
}

bool
p64_rwlock_try_acquire_rd(p64_rwlock_t *lock)
{
    p64_rwlock_t l;
    do
    {
	l = __atomic_load_n(lock, __ATOMIC_RELAXED);
	//Return immediately if writer present
	if ((l & RWLOCK_WRITER) != 0)
	{
	    return false;
	}
	//Attempt to increment number of readers
    }
    //A1: read lock.w, synchronize with A3
    while (!__atomic_compare_exchange_n(lock, &l, l + 1,
					/*weak=*/true,
					__ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE));
    return true;
}

void
p64_rwlock_release_rd(p64_rwlock_t *lock)
{
    p64_rwlock_t prevl;
    //Decrement number of readers
    //B0: write lock.r, synchronize with B1/B2
    prevl = __atomic_fetch_sub(lock, 1, __ATOMIC_RELEASE);
    //Check after lock is released but use pre-release lock value
    if (UNLIKELY((prevl & RWLOCK_READERS) == 0))
    {
	report_error("rwlock", "invalid read release", lock);
	return;
    }
}

void
p64_rwlock_acquire_wr(p64_rwlock_t *lock)
{
    p64_rwlock_t l;
    do
    {
	//Wait for any present writer to go away
	l = wait_for_no(lock, RWLOCK_WRITER, __ATOMIC_RELAXED);
	assert((l & RWLOCK_WRITER) == 0);

	//Attempt to set writer flag
    }
    //A2: read lock.w, synchronize with A3
    while (!__atomic_compare_exchange_n(lock, &l, l | RWLOCK_WRITER,
					/*weak=*/true,
					__ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE));

    //Wait for any present readers to go away
    //B1: read lock.r, synchronize with B0
    l = wait_for_no(lock, RWLOCK_READERS, __ATOMIC_ACQUIRE);
    assert(l == RWLOCK_WRITER);//One writer, no readers
}

bool
p64_rwlock_try_acquire_wr(p64_rwlock_t *lock)
{
    p64_rwlock_t l = __atomic_load_n(lock, __ATOMIC_RELAXED);
    //Lock must be completely free, we do not want to wait for any readers
    //to go away
    if (l == 0 &&
    //B2: read lock.r, synchronize with B0
	__atomic_compare_exchange_n(lock, &l, RWLOCK_WRITER,
				    /*weak=*/false,
				    __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE))
    {
	return true;
    }
    return false;
}

void
p64_rwlock_release_wr(p64_rwlock_t *lock)
{
    if (UNLIKELY(*lock != RWLOCK_WRITER))
    {
	report_error("rwlock", "invalid write release", lock);
	return;
    }
    //Clear writer flag
    //A3: write lock.w, synchronize with A0/A1/A2
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
}
