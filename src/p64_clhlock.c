// Copyright (c) 2017-2018 ARM Limited. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
//
// Original implementation by Brian Brooks @ ARM

#include <assert.h>
#include <stddef.h>
#include <stdlib.h>

#include "p64_clhlock.h"
#include "build_config.h"

#include "common.h"
#include "arch.h"
#include "inline.h"
#include "os_abstraction.h"
#include "err_hnd.h"
#include "atomic.h"

#define CLH_GO 0
#define CLH_WAIT 1

struct p64_clhnode
{
    struct p64_clhnode *prev;
    uint8_t wait;
};

static p64_clhnode_t *
alloc_clhnode(void)
{
    p64_clhnode_t *node = p64_malloc(sizeof(p64_clhnode_t), CACHE_LINE);
    if (node == NULL)
    {
	report_error("clhlock", "failed to allocate clhnode", 0);
	return NULL;
    }
    node->prev = NULL;
    node->wait = CLH_WAIT;
    return node;
}

void
p64_clhlock_init(p64_clhlock_t *lock)
{
    lock->tail = alloc_clhnode();
    if (lock->tail != NULL)
    {
	lock->tail->prev = NULL;
	lock->tail->wait = CLH_GO;
    }
}

void
p64_clhlock_fini(p64_clhlock_t *lock)
{
    p64_mfree(lock->tail);
}

static inline p64_clhnode_t *
enqueue(p64_clhlock_t *lock, p64_clhnode_t **nodep)
{
    p64_clhnode_t *node = *nodep;
    //When called first time, we will not have a node yet so allocate one
    if (node == NULL)
    {
	*nodep = node = alloc_clhnode();
    }
    node->wait = CLH_WAIT;

    //Insert our node last in queue, get back previous last (tail) node
    //A0: read and write tail, synchronize with A0
    p64_clhnode_t *prev = atomic_exchange_ptr(&lock->tail,
					      node,
					      __ATOMIC_ACQ_REL);

    //Save previous node in (what is still) "our" node for later use
    node->prev = prev;
    return prev;
}

void
p64_clhlock_acquire(p64_clhlock_t *lock, p64_clhnode_t **nodep)
{
    p64_clhnode_t *prev = enqueue(lock, nodep);

    //Wait for previous thread to signal us (using their node)
    //B0: read wait, synchronize with B1
    wait_until_equal(&prev->wait, CLH_GO, __ATOMIC_ACQUIRE);
    //Now we own the previous node
}

void
p64_clhlock_release(p64_clhnode_t **nodep)
{
    //Read previous node, it will become our new node
    p64_clhnode_t *prev = (*nodep)->prev;

    //Signal any (current or future) thread that waits for us using "our"
    //old node
    //B1: write wait, synchronize with B0
    atomic_store_n(&(*nodep)->wait, CLH_GO, __ATOMIC_RELEASE);
    //Now when we have signaled the next thread, it will own "our" old node

    //Save our new node for later use
    *nodep = prev;
}
