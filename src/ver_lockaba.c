//Copyright (c) 2025, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

//The lockaba module implements a simple spin lock with a subtle but fatal bug
//We want to demonstrate the verifier's ability to detect such bugs

#include <stdbool.h>
#include <stdlib.h>

#include "os_abstraction.h"
#include "verify.h"
#include "atomic.h"

#define NUMTHREADS 2

typedef uint32_t lock_t;
#define LOCK_FREE 0
#define LOCK_TAKEN 1

static void
lock_acquire(lock_t *lock)
{
    uint32_t cmp;
    do
    {
	//Wait until lock is available
	//Spin using load-acquire
	while (atomic_load_n(lock, __ATOMIC_ACQUIRE) != LOCK_FREE)
	{
	    //When spin-waiting, we need to ensure other threads get to run
	    VERIFY_YIELD();
	}
	//Now try to take lock
	cmp = LOCK_FREE;
	//Use relaxed memory ordering, what could go wrong?
    }
    while (!atomic_compare_exchange_n(lock, &cmp, LOCK_TAKEN, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
}

static void
lock_release(lock_t *lock)
{
    atomic_store_n(lock, LOCK_FREE, __ATOMIC_RELEASE);
}

static lock_t spin_lock;
static int32_t spin_owner;

static void
ver_lockaba_init(uint32_t numthreads)
{
    if (numthreads != NUMTHREADS)
    {
	abort();
    }
    spin_lock = LOCK_FREE;
    spin_owner = -1;
}

static void
ver_lockaba_fini(uint32_t numthreads)
{
    (void)numthreads;
    VERIFY_ASSERT(spin_owner == -1);
}

static void
ver_lockaba_exec(uint32_t id)
{
    (void)id;
    //Acquire the lock
    lock_acquire(&spin_lock);
    //Assert protected variable is not "owned" by any other thread
    VERIFY_ASSERT(regular_load_n(&spin_owner) == -1);
    //Now we "own" the protected variable
    regular_store_n(&spin_owner, id);
    //Assert we still own the protected variable
    VERIFY_ASSERT(regular_load_n(&spin_owner) == (int32_t)id);
    //We don't own the protected variable anymore
    regular_store_n(&spin_owner, -1);
    //Release the lock
    lock_release(&spin_lock);
    //And we are done
}

struct ver_funcs ver_lockaba =
{
    "lockaba", ver_lockaba_init, ver_lockaba_exec, ver_lockaba_fini
};
