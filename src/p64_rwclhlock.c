// Copyright (c) 2019 ARM Limited. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause

#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

#include "p64_rwclhlock.h"
#include "build_config.h"

#include "common.h"
#include "arch.h"
#include "os_abstraction.h"

#define WAIT 0
#define GO_ACQ 1
#define GO_REL 2

struct p64_rwclhnode
{
    struct p64_rwclhnode *prev;
    uint8_t reader;
    uint8_t ready;
};

static p64_rwclhnode_t *
alloc_rwclhnode(void)
{
    p64_rwclhnode_t *node = p64_malloc(sizeof(p64_rwclhnode_t), CACHE_LINE);
    if (node == NULL)
    {
	perror("p64_malloc");
	exit(EXIT_FAILURE);
    }
    node->prev = NULL;
    node->reader = false;
    node->ready = WAIT;
    return node;
}

void
p64_rwclhlock_init(p64_rwclhlock_t *lock)
{
    lock->tail = alloc_rwclhnode();
    lock->tail->prev = NULL;
    lock->tail->reader = false;
    lock->tail->ready = GO_REL;
}

void
p64_rwclhlock_fini(p64_rwclhlock_t *lock)
{
    p64_mfree(lock->tail);
}

static inline void
wait_for_go(uint8_t *loc, uint32_t lvl)
{
    //Wait for previous thread to signal us (using their node)
    if (__atomic_load_n(loc, __ATOMIC_ACQUIRE) < lvl)
    {
	SEVL();
	while (WFE() && LDXR8(loc, __ATOMIC_ACQUIRE) < lvl)
	{
	    DOZE();
	}
    }
}

static inline p64_rwclhnode_t *
enqueue(p64_rwclhlock_t *lock, p64_rwclhnode_t **nodep, bool reader)
{
    //When called first time, we will not have a node yet so allocate one
    if (*nodep == NULL)
    {
	*nodep = alloc_rwclhnode();
    }
    (*nodep)->reader = reader;
    (*nodep)->ready = WAIT;

    //Insert our node last in queue, get back previous last (tail) node
    p64_rwclhnode_t *prev = __atomic_exchange_n(&lock->tail,
						*nodep,
						__ATOMIC_ACQ_REL);

    //Save previous node in (what is still) "our" node for later use
    (*nodep)->prev = prev;
    return prev;
}

void
p64_rwclhlock_acquire_rd(p64_rwclhlock_t *lock, p64_rwclhnode_t **nodep)
{
    p64_rwclhnode_t *prev = enqueue(lock, nodep, true);
    if (prev->reader)
    {
	//Wait for previous reader to signal us (using their node)
	wait_for_go(&prev->ready, GO_ACQ);

	//Signal any later readers
	__atomic_store_n(&(*nodep)->ready, GO_ACQ, __ATOMIC_RELAXED);
    }
    else
    {
	//Wait for previous writer to signal us (using their node)
	wait_for_go(&prev->ready, GO_REL);
    }
}

void
p64_rwclhlock_release_rd(p64_rwclhnode_t **nodep)
{
    //Read previous node, it will become our new node
    p64_rwclhnode_t *prev = (*nodep)->prev;

    if (prev->reader)
    {
	//Wait for previous reader to signal us (using their node)
	wait_for_go(&prev->ready, GO_REL);
	//Now we own the previous node
    }

    //Signal later readers and writers that we are done
    __atomic_store_n(&(*nodep)->ready, GO_REL, __ATOMIC_RELEASE);
    //Now when we have signaled the next thread, it will own "our" old node

    //Save our new node for later use
    *nodep = prev;
}

void
p64_rwclhlock_acquire_wr(p64_rwclhlock_t *lock, p64_rwclhnode_t **nodep)
{
    p64_rwclhnode_t *prev = enqueue(lock, nodep, false);
    //Wait for previous reader or writer to signal us (using their node)
    wait_for_go(&prev->ready, GO_REL);
    //Now we own the previous node
}

void
p64_rwclhlock_release_wr(p64_rwclhnode_t **nodep)
{
    //Read previous node, it will become our new node
    p64_rwclhnode_t *prev = (*nodep)->prev;

    //Signal later readers and writers that we are done
    __atomic_store_n(&(*nodep)->ready, GO_REL, __ATOMIC_RELEASE);
    //Now when we have signaled the next thread, it will own "our" old node

    //Save our new node for later use
    *nodep = prev;
}
