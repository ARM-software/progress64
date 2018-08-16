// Copyright (c) 2017-2018 ARM Limited. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
//
// Implementation courtesy of Brian Brooks @ ARM

#include <stddef.h>

#include "p64_clhlock.h"
#include "build_config.h"

#include "common.h"
#include "arch.h"

void
p64_clhlock_init(p64_clhlock_t *lock)
{
    lock->node.prev = NULL;
    lock->node.wait = 0;
    lock->tail = &lock->node;
}

void
p64_clhlock_acquire(p64_clhlock_t *lock, p64_clhnode_t *node)
{
    node->wait = 1;

    p64_clhnode_t *prev =
	node->prev = __atomic_exchange_n(&lock->tail, node, __ATOMIC_ACQ_REL);

    if (__atomic_load_n(&prev->wait, __ATOMIC_ACQUIRE))
    {
	SEVL();
	while (WFE() && LDXR32(&prev->wait, __ATOMIC_ACQUIRE))
	{
	    DOZE();
	}
    }
}

void
p64_clhlock_release(p64_clhnode_t **nodep)
{
    p64_clhnode_t *prev = (*nodep)->prev;

#ifdef USE_DMB
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&(*nodep)->wait, 0, __ATOMIC_RELAXED);
#else
    __atomic_store_n(&(*nodep)->wait, 0, __ATOMIC_RELEASE);
#endif
    *nodep = prev;
}
