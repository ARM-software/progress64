// Copyright (c) 2019,2023 ARM Limited. All rights reserved.
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
	//Error has already been reported
	return;
    }
    lock->tail->prev = NULL;
    lock->tail->futex = SIGNAL_REL;
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
	while ((int)LDX((unsigned *)loc, __ATOMIC_ACQUIRE) < sig)
	{
	    WFE();
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
	doze();
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
					__ATOMIC_ACQUIRE,
					__ATOMIC_ACQUIRE))
	{
	    //CAS succeeded, previous thread must wake us up so sleep now
	    futex_wait(loc, wakeup);
	    //Get fresh value of location
	    actual = __atomic_load_n(loc, __ATOMIC_ACQUIRE);
	}
	//Else CAS failed, actual updated
    }
    while (actual < sig);
}

static void
signal_next(int *loc, int sig)
{
    assert(sig == SIGNAL_ACQ || sig == SIGNAL_REL);
    int old = WAIT;
    if (__atomic_compare_exchange_n(loc,
				    &old,
				    sig,
				    0,//strong
				    __ATOMIC_RELEASE,
				    __ATOMIC_RELAXED))
    {
	return;
    }
    //Else old updated
    do
    {
	if (old == WAKE_REL && sig == SIGNAL_ACQ)
	{
	    //Next thread waiting for SIGNAL_REL
	    //Don't wake up now, wait until we signal SIGNAL_REL
	    return;
	}
    }
    while (!__atomic_compare_exchange_n(loc,
					&old,
					sig,
					0,//strong
					__ATOMIC_RELEASE,
					__ATOMIC_RELAXED));
    //CAS succeeded, '*loc' updated
    if (old == WAKE_ACQ || old == WAKE_REL)
    {
	assert(sig >= old - 2);
	futex_wake(loc);
    }
}

static p64_rwclhnode_t *
enqueue(p64_rwclhlock_t *lock, p64_rwclhnode_t **nodep)
{
    p64_rwclhnode_t *node = *nodep;
    //When called the first time, we will not yet have a node so allocate one
    if (node == NULL)
    {
	*nodep = node = alloc_rwclhnode(lock);
	if (UNLIKELY(node == NULL))
	{
	    //Error has already been reported
	    return NULL;
	}
	node->spin_tmo = lock->spin_tmo;
    }
    node->prev = NULL;
    node->futex = WAIT;

    //Insert our node last in queue, get back previous last (tail) node
    //Q0: read and write lock.tail, synchronize with Q0
    p64_rwclhnode_t *prev = __atomic_exchange_n(&lock->tail,
						node,
						__ATOMIC_ACQ_REL);

    return prev;
}

void
p64_rwclhlock_acquire_rd(p64_rwclhlock_t *lock, p64_rwclhnode_t **nodep)
{
    p64_rwclhnode_t *prev = enqueue(lock, nodep);
    if (UNLIKELY(prev == NULL))
    {
	//Error has already been reported
	return;
    }
    //Save previous node in our node for later use
    p64_rwclhnode_t *node = *nodep;
    node->prev = prev;

    //Wait for previous thread to signal us
    //A0: read futex waiting for ACQ (or REL), synchronize with A1/R3
    wait_prev(&prev->futex, SIGNAL_ACQ, node->spin_tmo);

    //Signal any later readers (using our node)
    //A1: write futex with ACQ, synchronize with A0
    signal_next(&node->futex, SIGNAL_ACQ);
}

void
p64_rwclhlock_release_rd(p64_rwclhnode_t **nodep)
{
    p64_rwclhnode_t *node = *nodep;
    //Read previous node, it will become our new node
    p64_rwclhnode_t *prev = node->prev;

    //Wait for previous thread to release the lock
    //R0: read futex waiting for REL, synchronize with R1
    wait_prev(&prev->futex, SIGNAL_REL, node->spin_tmo);
    //Now we own the previous node

    //Signal the next thread that we are done
    //R1: write futex with REL, synchronize with R0/R2
    signal_next(&node->futex, SIGNAL_REL);
    //Now when we have signaled the next thread, it will own our old node

    //Save our new node for later use
    *nodep = prev;
}

void
p64_rwclhlock_acquire_wr(p64_rwclhlock_t *lock, p64_rwclhnode_t **nodep)
{
    p64_rwclhnode_t *prev = enqueue(lock, nodep);
    if (UNLIKELY(prev == NULL))
    {
	//Error has already been reported
	return;
    }
    p64_rwclhnode_t *node = *nodep;
    //Save previous node in our node for later use
    node->prev = prev;

    //Wait for previous thread to release the lock
    //R2: read futex waiting for REL, synchronize with R1/R3
    wait_prev(&prev->futex, SIGNAL_REL, node->spin_tmo);
    //Now we own the previous node
}

void
p64_rwclhlock_release_wr(p64_rwclhnode_t **nodep)
{
    p64_rwclhnode_t *node = *nodep;
    //Read previous node, it will become our new node
    p64_rwclhnode_t *prev = node->prev;

    //Signal next thread that we are done
    //R3: write futex with REL, synchronize with R2/A0
    signal_next(&node->futex, SIGNAL_REL);
    //Now when we have signaled the next thread, it will own our old node

    //Save our new node for later use
    *nodep = prev;
}
