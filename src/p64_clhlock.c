// Copyright (c) 2017-2018 ARM Limited. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
//
// Original implementation by Brian Brooks @ ARM

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "p64_clhlock.h"
#include "build_config.h"

#include "common.h"
#include "arch.h"
#include "os_abstraction.h"

struct p64_clhnode
{
    struct p64_clhnode *prev;
    uint32_t wait;
};

static p64_clhnode_t *
alloc_clhnode(void)
{
    p64_clhnode_t *node = p64_malloc(sizeof(p64_clhnode_t), CACHE_LINE);
    if (node == NULL)
    {
	perror("p64_malloc");
	exit(EXIT_FAILURE);
    }
    node->prev = NULL;
    __atomic_store_n(&node->wait, 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return node;
}

void
p64_clhlock_init(p64_clhlock_t *lock)
{
    lock->tail = alloc_clhnode();
    lock->tail->prev = NULL;
    __atomic_store_n(&lock->tail->wait, 0, __ATOMIC_RELAXED);
}

void
p64_clhlock_fini(p64_clhlock_t *lock)
{
    p64_mfree(lock->tail);
}

void
p64_clhlock_acquire(p64_clhlock_t *lock, p64_clhnode_t **nodep)
{
    PREFETCH_FOR_WRITE(&lock->tail);

    //When called first time, we will not have a node yet so allocate one
    if (*nodep == NULL)
    {
	*nodep = alloc_clhnode();
    }
    assert((*nodep)->wait == 1);

    //Insert our node last in queue, get back previous last (tail) node
    p64_clhnode_t *prev = __atomic_exchange_n(&lock->tail,
					      *nodep,
					      __ATOMIC_ACQUIRE);

    //Save previous node in (what is still) "our" node for later use
    (*nodep)->prev = prev;

    //Wait for previous thread to signal us (using their node)
    if (__atomic_load_n(&prev->wait, __ATOMIC_ACQUIRE))
    {
	SEVL();
	while (WFE() && LDXR32(&prev->wait, __ATOMIC_ACQUIRE))
	{
	    DOZE();
	}
    }
    //Now we own the previous node

    //Ensure the next thread will wait for us
    __atomic_store_n(&prev->wait, 1, __ATOMIC_RELAXED);
}

void
p64_clhlock_release(p64_clhnode_t **nodep)
{
    //Read previous node, it will become our new node
    p64_clhnode_t *prev = (*nodep)->prev;

    //Signal any (current or future) thread that waits for us using "our"
    //old node
#ifdef USE_DMB
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&(*nodep)->wait, 0, __ATOMIC_RELAXED);
#else
    __atomic_store_n(&(*nodep)->wait, 0, __ATOMIC_RELEASE);
#endif
    //Now when we have signaled the next thread, it will own "our" old node

    //Save our new node for later use
    *nodep = prev;
}
