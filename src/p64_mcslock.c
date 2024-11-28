// Copyright (c) 2020 ARM Limited. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause

#include <stddef.h>

#include "p64_mcslock.h"
#include "build_config.h"

#include "common.h"
#include "arch.h"

#define MCS_GO 0
#define MCS_WAIT 1

void
p64_mcslock_init(p64_mcslock_t *lock)
{
    *lock = NULL;
}

void
p64_mcslock_acquire(p64_mcslock_t *lock, p64_mcsnode_t *node)
{
    node->next = NULL;
    //A0: read and write lock, synchronize with A0/A1
    p64_mcsnode_t *prev = __atomic_exchange_n(lock, node, __ATOMIC_ACQ_REL);
    if (LIKELY(prev == NULL))
    {
	//Lock uncontended, we have acquired it
	return;
    }
    //Else lock owned by other thread, we must wait for our turn

    node->wait = MCS_WAIT;
    //B0: write next, synchronize with B1/B2
    __atomic_store_n(&prev->next, node, __ATOMIC_RELEASE);

    //Wait for previous thread to signal us (using our node)
    //C0: read wait, synchronize with C1
    wait_until_equal(&node->wait, MCS_GO, __ATOMIC_ACQUIRE);
}

void
p64_mcslock_release(p64_mcslock_t *lock, p64_mcsnode_t *node)
{
    p64_mcsnode_t *next;
    //Check if there is any waiting thread
    //B1: read next, synchronize with B0
    if ((next = __atomic_load_n(&node->next, __ATOMIC_ACQUIRE)) == NULL)
    {
	//Seems there are no waiting threads, try to release lock
	p64_mcsnode_t *tmp = node;//Use temporary variable since it might get overwritten
	//A1: write lock, synchronize with A0
	if (__atomic_compare_exchange_n(lock, &tmp, NULL, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED))
	{
	    //Still no waiters, lock released successfully
	    return;
	}
	//Else there is at least one waiting thread

	//Wait for first waiting thread to link their node to our node
	//B2: read next, synchronize with B0
	while ((next = __atomic_load_n(&node->next, __ATOMIC_ACQUIRE)) == NULL)
	{
	    doze();
	}
    }
    //Signal first waiting thread
    //C1: write wait, synchronize with C0
    __atomic_store_n(&next->wait, MCS_GO, __ATOMIC_RELEASE);
}
