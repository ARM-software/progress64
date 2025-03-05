// Copyright (c) 2025 ARM Limited. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "p64_rplock.h"

#include "atomic.h"
#include "verify.h"

#define LOCKEDEMPTY (p64_rpnode_t *)1 //An otherwise invalid pointer

void
p64_rplock_init(p64_rplock_t *lock)
{
    lock->arrivals = NULL;
}

bool
p64_rplock_try_acquire(p64_rplock_t *lock, p64_rpnode_t *node)
{
    regular_store_n(&node->gate, NULL);
    node->succ = NULL;
    node->eos = node;
    //A0r: read arrivals, synchronize with A0w/A2
    //A0w: write arrivals, synchronize with A0r/A1
    p64_rpnode_t *tail = NULL;
    if (!atomic_compare_exchange_ptr(&lock->arrivals,
				     &tail,
				     node,
				     __ATOMIC_ACQ_REL,
				     __ATOMIC_ACQUIRE))
    {
	return false;
    }
    assert(tail != node);
    assert(node->eos != NULL);
    assert(atomic_load_ptr(&lock->arrivals, __ATOMIC_RELAXED) != NULL);
    return true;
}

void
p64_rplock_acquire(p64_rplock_t *lock, p64_rpnode_t *node)
{
    regular_store_n(&node->gate, NULL);
    node->succ = NULL;
    node->eos = node;
    //A0r: read arrivals, synchronize with A0w/A2
    //A0w: write arrivals, synchronize with A0r/A1
    p64_rpnode_t *tail = atomic_exchange_ptr(&lock->arrivals, node, __ATOMIC_ACQ_REL);
    assert(tail != node);
    if (tail != NULL)
    {
	node->succ = (p64_rpnode_t *)((uintptr_t)tail & ~(uintptr_t)1);
	assert(node->succ != node);
	//B0: read gate, synchronize with B1
	node->eos = wait_until_not_equal_ptr(&node->gate, NULL, __ATOMIC_ACQUIRE);
	assert(node->eos != node);
	if (node->succ == node->eos)
	{
	    node->succ = NULL;
	    node->eos = LOCKEDEMPTY;
	}
    }
    assert(node->eos != NULL);
    assert(atomic_load_ptr(&lock->arrivals, __ATOMIC_RELAXED) != NULL);
}

void
p64_rplock_release(p64_rplock_t *lock, p64_rpnode_t *node)
{
    //Case: entry list populated, appoint successor from entry list
    if (node->succ != NULL)
    {
	assert(node->eos != node && node->succ->gate == NULL);
	VERIFY_ASSERT(regular_load_n(&node->succ->gate) == NULL);
	atomic_store_n(&node->succ->gate, node->eos, __ATOMIC_RELEASE);
	return;
    }
    //Case: entry list and arrivals both empty, try fast-path uncontended unload
    assert(node->eos == LOCKEDEMPTY || node->eos == node);
    p64_rpnode_t *v = node->eos;
    //A2: (read/)write arrivals, synchronize with A0r
    if (atomic_compare_exchange_ptr(&lock->arrivals,
				    &v,
				    NULL,
				    __ATOMIC_RELEASE,
				    __ATOMIC_RELAXED))
    {
	return;
    }
    //Else CASE failed, arrivals list contains nodes from other threads

    //Case: entry list is empty and arrivals is populated
    //New threads have arrived and pushed themselves onto the arrival stack
    //We now detach that segment, shifting those new arrivals to become the next entry segment
    //A1: read(/write) arrivals, synchronize with A0w
    //TODO Do we need release semantics as well? Or passing the lock is handled by B1 below?
    p64_rpnode_t *w = atomic_exchange_ptr(&lock->arrivals, LOCKEDEMPTY, __ATOMIC_ACQUIRE);
    assert(w != NULL && w != LOCKEDEMPTY && w != node);
    assert(w->gate == NULL);
    VERIFY_ASSERT(regular_load_n(&w->gate) == NULL);
    //B1: write gate, synchronize with B0
    atomic_store_ptr(&w->gate, node->eos, __ATOMIC_RELEASE);
}
