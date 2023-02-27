// Copyright (c) 2019 ARM Limited. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <errno.h>
#ifdef __linux__
#include <linux/futex.h>
#endif
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#include <unistd.h>

#include "p64_rwclhlock.h"
#include "build_config.h"

#include "common.h"
#include "arch.h"
#include "os_abstraction.h"
#include "err_hnd.h"

#ifdef __linux__
static inline int
futex(int *uaddr,
      int op,
      int val,
      const struct timespec *ts,
      int *uaddr2, int val2)
{
    (void)uaddr2;
    return syscall(SYS_futex, uaddr, op | FUTEX_PRIVATE_FLAG,
		   val, ts, uaddr, val2);
}
#endif

static inline void
futex_wait(int *loc, int val)
{
#ifdef __linux__
    if (futex(loc, FUTEX_WAIT, val, NULL, NULL, 0) < 0 && errno != EAGAIN)
    {
	perror("futex(WAIT)");
	abort();
    }
#else
    (void)loc;
    (void)val;
#endif
}

static inline void
futex_wake(int *loc)
{
#ifdef __linux__
    if (futex(loc, FUTEX_WAKE, 1, NULL, NULL, 0) < 0)
    {
	perror("futex(WAKE)");
	abort();
    }
#else
    (void)loc;
#endif
}

#define WAIT 0
#define SIGNAL_ACQ 1
#define SIGNAL_REL 2
#define WAKE_ACQ (SIGNAL_ACQ + 2)
#define WAKE_REL (SIGNAL_REL + 2)

struct p64_rwclhnode
{
    struct p64_rwclhnode *prev;
    uint32_t spin_tmo;
    int futex;
    bool reader;
};

static p64_rwclhnode_t *
alloc_rwclhnode(p64_rwclhlock_t *lock)
{
    p64_rwclhnode_t *node = p64_malloc(sizeof(p64_rwclhnode_t), CACHE_LINE);
    if (UNLIKELY(node == NULL))
    {
	report_error("rwclh", "failed to allocate rwclhnode", lock);
	return NULL;
    }
    return node;
}

void
p64_rwclhlock_init(p64_rwclhlock_t *lock, uint32_t spin_tmo_ns)
{
    lock->tail = alloc_rwclhnode(lock);
    if (lock->tail == NULL)
    {
	return;
    }
    lock->tail->prev = NULL;
    lock->tail->futex = SIGNAL_REL;
    lock->tail->reader = false;
    uint32_t spin_tmo = spin_tmo_ns;
    if (spin_tmo_ns != P64_RWCLHLOCK_SPIN_FOREVER)
    {
	spin_tmo = spin_tmo_ns * counter_freq() / 1000000000;
    }
    lock->tail->spin_tmo = spin_tmo;
    lock->spin_tmo = spin_tmo;
}

void
p64_rwclhlock_fini(p64_rwclhlock_t *lock)
{
    p64_mfree(lock->tail);
}

//Wait for previous thread to signal us (using their node)
static void
wait_prev(int *loc, int sig, uint32_t spin_tmo)
{
    int actual = __atomic_load_n(loc, __ATOMIC_ACQUIRE);
    if (actual >= sig)
    {
	return;
    }
    //Check for infinite spinning
    if (spin_tmo == P64_RWCLHLOCK_SPIN_FOREVER)
    {
	//Wait indefinite time using Wait-For-Event
	//Handle spurious wake-ups
	SEVL();
	while (WFE() && (int)LDX((unsigned *)loc, __ATOMIC_ACQUIRE) < sig)
	{
	    DOZE();
	}
	return;
    }
    //Spin until timeout
    uint64_t start = counter_read();
    while (counter_read() - start < spin_tmo)
    {
	actual = __atomic_load_n(loc, __ATOMIC_ACQUIRE);
	if (actual >= sig)
	{
	    return;
	}
	DOZE();
    }
    //Spinning timed out
    //Tell previous thread to wake us up from sleep
    do
    {
	assert(actual != WAKE_ACQ && actual != WAKE_REL);
	int wakeup = sig + 2;
	if (__atomic_compare_exchange_n(loc,
					&actual,
					wakeup,
					0,//strong
					__ATOMIC_RELAXED,
					__ATOMIC_RELAXED))
	{
	    //CAS succeeded, previous thread must wake us up
	    futex_wait(loc, wakeup);
	}
	//CAS failed
	actual = __atomic_load_n(loc, __ATOMIC_ACQUIRE);
    }
    while (actual < sig);
}

static void
signal_next(int *loc, int sig, int mo, uint32_t spin_tmo)
{
    assert(sig == SIGNAL_ACQ || sig == SIGNAL_REL);
    if (spin_tmo == P64_RWCLHLOCK_SPIN_FOREVER)
    {
	__atomic_store_n(loc, sig, mo);
	return;
    }
    int old = __atomic_load_n(loc, __ATOMIC_RELAXED);
    do
    {
	if (old == WAKE_REL && sig == SIGNAL_ACQ)
	{
	    //Don't wake up now, wait until we signal SIGNAL_REL
	    return;
	}
    }
    while (!__atomic_compare_exchange_n(loc,
					&old,
					sig,
					0,//strong
					mo,
					__ATOMIC_RELAXED));
    //CAS succeeded, '*loc' updated
    if (old == WAKE_ACQ || old == WAKE_REL)
    {
	assert(sig >= old - 2);
	futex_wake(loc);
    }
}

static p64_rwclhnode_t *
enqueue(p64_rwclhlock_t *lock, p64_rwclhnode_t **nodep, bool reader)
{
    p64_rwclhnode_t *node = *nodep;
    //When called first time, we will not have a node yet so allocate one
    if (node == NULL)
    {
	*nodep = node = alloc_rwclhnode(lock);
	if (UNLIKELY(node == NULL))
	{
	    return NULL;
	}
	node->spin_tmo = lock->spin_tmo;
    }
    node->prev = NULL;
    node->futex = WAIT;
    node->reader = reader;

    //Insert our node last in queue, get back previous last (tail) node
    p64_rwclhnode_t *prev = __atomic_exchange_n(&lock->tail,
						node,
						__ATOMIC_ACQ_REL);

    //Save previous node in (what is still) "our" node for later use
    node->prev = prev;
    return prev;
}

void
p64_rwclhlock_acquire_rd(p64_rwclhlock_t *lock, p64_rwclhnode_t **nodep)
{
    p64_rwclhnode_t *prev = enqueue(lock, nodep, true);

    if (prev->reader)
    {
	p64_rwclhnode_t *node = *nodep;

	//Wait for previous thread to signal us (using their node)
	wait_prev(&prev->futex, SIGNAL_ACQ, prev->spin_tmo);

	//Signal any later readers
	signal_next(&node->futex, SIGNAL_ACQ, __ATOMIC_RELAXED, prev->spin_tmo);
    }
    else
    {
	//Wait for previous writer to signal us (using their node)
	wait_prev(&prev->futex, SIGNAL_REL, prev->spin_tmo);
    }
    //Now we own the previous node
}

void
p64_rwclhlock_release_rd(p64_rwclhnode_t **nodep)
{
    p64_rwclhnode_t *node = *nodep;

    //Read previous node, it will become our new node
    p64_rwclhnode_t *prev = node->prev;

    if (prev->reader)
    {
	//Wait for previous reader to signal us (using their node)
	wait_prev(&prev->futex, SIGNAL_REL, prev->spin_tmo);
	//Now we own the previous node
    }

    //Signal later readers and writers that we are done
    signal_next(&node->futex, SIGNAL_REL, __ATOMIC_RELEASE, prev->spin_tmo);
    //Now when we have signaled the next thread, it will own "our" old node

    //Save our new node for later use
    *nodep = prev;
}

void
p64_rwclhlock_acquire_wr(p64_rwclhlock_t *lock, p64_rwclhnode_t **nodep)
{
    p64_rwclhnode_t *prev = enqueue(lock, nodep, true);
    //Wait for previous writer to signal us (using their node)
    wait_prev(&prev->futex, SIGNAL_REL, prev->spin_tmo);
    //Now we own the previous node
}

void
p64_rwclhlock_release_wr(p64_rwclhnode_t **nodep)
{
    p64_rwclhnode_t *node = *nodep;

    //Read previous node, it will become our new node
    p64_rwclhnode_t *prev = node->prev;

    //Signal later readers and writers that we are done
    signal_next(&node->futex, SIGNAL_REL, __ATOMIC_RELEASE, prev->spin_tmo);
    //Now when we have signaled the next thread, it will own "our" old node

    //Save our new node for later use
    *nodep = prev;
}
