//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "p64_rwlock_r.h"

#include "common.h"
#include "os_abstraction.h"

//Must not be larger than number of bits in release_mask below
#define STACKSIZE 32

void
p64_rwlock_r_init(p64_rwlock_r_t *lock)
{
    p64_rwlock_init(&lock->rwlock);
    lock->owner = INVALID_TID;
}

static THREAD_LOCAL struct
{
    uint64_t threadid;
    uint32_t release_mask;
    uint32_t depth;
    p64_rwlock_r_t *stack[STACKSIZE];
} pth = { INVALID_TID, 0, 0, { NULL } };

static bool
find_lock(p64_rwlock_r_t *lock)
{
    for (uint32_t i = 0; i < pth.depth; i++)
    {
	if (pth.stack[i] == lock)
	{
	    return true;
	}
    }
    return false;
}

void
p64_rwlock_r_acquire_rd(p64_rwlock_r_t *lock)
{
    if (UNLIKELY(pth.threadid == INVALID_TID))
    {
	pth.threadid = p64_gettid();
    }
    if (UNLIKELY(pth.depth == STACKSIZE))
    {
	fprintf(stderr, "rwlock_r: too many calls p64_rwlock_r_acquire_rd/wr\n");
	fflush(stderr);
	abort();
    }
    if (!find_lock(lock))
    {
	//First time this specific lock is acquired
	p64_rwlock_acquire_rd(&lock->rwlock);
	//It must be released later
	pth.release_mask |= 1UL << pth.depth;
    }
    pth.stack[pth.depth++] = lock;
}

bool
p64_rwlock_r_try_acquire_rd(p64_rwlock_r_t *lock)
{
    if (UNLIKELY(pth.threadid == INVALID_TID))
    {
	pth.threadid = p64_gettid();
    }
    if (UNLIKELY(pth.depth == STACKSIZE))
    {
	fprintf(stderr, "rwlock_r: too many calls p64_rwlock_r_acquire_rd/wr\n");
	fflush(stderr);
	abort();
    }
    if (!find_lock(lock))
    {
	//First time this specific lock is acquired
	if (!p64_rwlock_try_acquire_rd(&lock->rwlock))
	{
	    return false;
	}
	//It must be released later
	pth.release_mask |= 1UL << pth.depth;
    }
    pth.stack[pth.depth++] = lock;
    return true;
}

void
p64_rwlock_r_release_rd(p64_rwlock_r_t *lock)
{
    if (UNLIKELY(pth.depth == 0))
    {
	fprintf(stderr, "rwlock_r: superfluous call to p64_rwlock_r_release_rd()\n");
	fflush(stderr);
	abort();
    }
    else if (UNLIKELY(pth.stack[pth.depth - 1] != lock))
    {
	fprintf(stderr, "rwlock_r: p64_rwlock_r_release_rd() called for wrong lock\n");
	fflush(stderr);
	abort();
    }
    pth.depth--;
    if (pth.release_mask & (1UL << pth.depth))
    {
	pth.release_mask &= ~(1UL << pth.depth);
	p64_rwlock_release_rd(&lock->rwlock);
    }
}

void
p64_rwlock_r_acquire_wr(p64_rwlock_r_t *lock)
{
    if (UNLIKELY(pth.threadid == INVALID_TID))
    {
	pth.threadid = p64_gettid();
    }
    if (UNLIKELY(pth.depth == STACKSIZE))
    {
	fprintf(stderr, "rwlock_r: too many calls p64_rwlock_r_acquire_rd/wr\n");
	fflush(stderr);
	abort();
    }
    if (__atomic_load_n(&lock->owner, __ATOMIC_RELAXED) != pth.threadid)
    {
	if (UNLIKELY(find_lock(lock)))
	{
	    fprintf(stderr, "rwlock_r: acquire-write after acquire-read\n");
	    fflush(stderr);
	    abort();
	}
	p64_rwlock_acquire_wr(&lock->rwlock);
	__atomic_store_n(&lock->owner, pth.threadid, __ATOMIC_RELAXED);
	//First time this specific lock is acquired so it must be released later
	pth.release_mask |= 1UL << pth.depth;
    }
    pth.stack[pth.depth++] = lock;
}

bool
p64_rwlock_r_try_acquire_wr(p64_rwlock_r_t *lock)
{
    if (UNLIKELY(pth.threadid == INVALID_TID))
    {
	pth.threadid = p64_gettid();
    }
    if (UNLIKELY(pth.depth == STACKSIZE))
    {
	fprintf(stderr, "rwlock_r: too many calls p64_rwlock_r_acquire_rd/wr\n");
	fflush(stderr);
	abort();
    }
    if (__atomic_load_n(&lock->owner, __ATOMIC_RELAXED) != pth.threadid)
    {
	if (UNLIKELY(find_lock(lock)))
	{
	    //acquire-write after acquire-read not allowed
	    return false;
	}
	if (!p64_rwlock_try_acquire_wr(&lock->rwlock))
	{
	    return false;
	}
	__atomic_store_n(&lock->owner, pth.threadid, __ATOMIC_RELAXED);
	//First time this specific lock is acquired so it must be released later
	pth.release_mask |= 1UL << pth.depth;
    }
    pth.stack[pth.depth++] = lock;
    return true;
}

void
p64_rwlock_r_release_wr(p64_rwlock_r_t *lock)
{
    if (UNLIKELY(pth.depth == 0))
    {
	fprintf(stderr, "rwlock_r: superfluous call to p64_rwlock_r_release_wr()\n");
	fflush(stderr);
	abort();
    }
    else if (UNLIKELY(pth.stack[pth.depth - 1] != lock))
    {
	fprintf(stderr, "rwlock_r: p64_rwlock_r_release_wr() called for wrong lock\n");
	fflush(stderr);
	abort();
    }
    pth.depth--;
    if (pth.release_mask & (1UL << pth.depth))
    {
	pth.release_mask &= ~(1UL << pth.depth);
	__atomic_store_n(&lock->owner, INVALID_TID, __ATOMIC_RELAXED);
	p64_rwlock_release_wr(&lock->rwlock);
    }
}
