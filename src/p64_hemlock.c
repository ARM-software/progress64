// Copyright (c) 2023 ARM Limited. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause

#include <assert.h>
#include <stddef.h>

#include "p64_hemlock.h"
#include "build_config.h"

#include "common.h"
#include "arch.h"
#include "atomic.h"
#include "verify.h"

static THREAD_LOCAL struct p64_hemlock *grant = NULL;

void
p64_hemlock_init(p64_hemlock_t *lock)
{
    lock->tail = NULL;
}

bool
p64_hemlock_try_acquire(p64_hemlock_t *lock)
{
    assert(grant == NULL);
    //lock->tail == NULL => lock is free
    struct p64_hemlock **pred = NULL;
    //A0: read and write tail, synchronize with A0/A1/A2
    return atomic_compare_exchange_ptr(&lock->tail, &pred, &grant,
				       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

void
p64_hemlock_acquire(p64_hemlock_t *lock)
{
    assert(grant == NULL);
    //A1: read and write tail, synchronize with A0/A1/A2
    struct p64_hemlock **pred =
	atomic_exchange_ptr(&lock->tail, &grant, __ATOMIC_ACQ_REL);
    if (LIKELY(pred == NULL))
    {
	//Lock uncontended, we have acquired it
	return;
    }
    //Else lock owned by other thread, we must wait for our turn

    //Wait for previous thread to signal us
    //B0: read pred, synchronize with B1
    wait_until_equal_ptr(pred, lock, __ATOMIC_ACQUIRE);

    //Ack grant, grant can be reused
    //C0: write pred, signal C1
    atomic_store_ptr(pred, NULL, __ATOMIC_RELAXED);
}

void
p64_hemlock_release(p64_hemlock_t *lock)
{
    assert(grant == NULL);
    //Lock->tail == &grant => no waiters
    struct p64_hemlock **v = &grant;
    //A2: write tail, synchronize with A0/A1
    if (atomic_compare_exchange_ptr(&lock->tail, &v, NULL,
				    __ATOMIC_RELEASE, __ATOMIC_RELAXED))
    {
	//No waiting threads
	return;
    }
    //Else there is at least one waiting thread

    //Signal first waiting thread which is polling our grant
    //B1: write pred, synchronize with B0
    atomic_store_ptr(&grant, lock, __ATOMIC_RELEASE);

    //Wait for thread to ack the grant
    //C1: read pred, wait-on C0
    wait_until_equal_ptr(&grant, NULL, __ATOMIC_RELAXED);
    //Potentially, this poll loop could be moved to the beginning of the acquire and release
    //functions. That would overlap the waiting with execution outside of the critical section.
}
