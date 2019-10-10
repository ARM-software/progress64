//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "p64_tfrwlock_r.h"

#include "common.h"
#include "os_abstraction.h"

//Must not be larger than number of bits in release_mask below
#define STACKSIZE 32

void
p64_tfrwlock_r_init(p64_tfrwlock_r_t *lock)
{
    p64_tfrwlock_init(&lock->tfrwlock);
    lock->owner = INVALID_TID;
}

static THREAD_LOCAL struct
{
    uint64_t threadid;
    uint32_t release_mask;
    uint32_t depth;
    uint16_t tkts[STACKSIZE];
    p64_tfrwlock_r_t *stack[STACKSIZE];
} pth = { INVALID_TID, 0, 0, { 0 }, { NULL } };

static bool
find_lock(p64_tfrwlock_r_t *lock)
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
p64_tfrwlock_r_acquire_rd(p64_tfrwlock_r_t *lock)
{
    if (UNLIKELY(pth.threadid == INVALID_TID))
    {
	pth.threadid = p64_gettid();
    }
    if (UNLIKELY(pth.depth == STACKSIZE))
    {
	fprintf(stderr, "tfrwlock_r: too many calls p64_tfrwlock_r_acquire_rd/wr\n");
	fflush(stderr);
	abort();
    }
    if (!find_lock(lock))
    {
	//First time this specific lock is acquired
	p64_tfrwlock_acquire_rd(&lock->tfrwlock);
	//It must be released later
	pth.release_mask |= UINT32_C(1) << pth.depth;
    }
    pth.stack[pth.depth++] = lock;
}

void
p64_tfrwlock_r_release_rd(p64_tfrwlock_r_t *lock)
{
    if (UNLIKELY(pth.depth == 0))
    {
	fprintf(stderr, "tfrwlock_r: superfluous call to p64_tfrwlock_r_release_rd()\n");
	fflush(stderr);
	abort();
    }
    else if (UNLIKELY(pth.stack[pth.depth - 1] != lock))
    {
	fprintf(stderr, "tfrwlock_r: p64_tfrwlock_r_release_rd() called for wrong lock\n");
	fflush(stderr);
	abort();
    }
    pth.depth--;
    if (pth.release_mask & (UINT32_C(1) << pth.depth))
    {
	pth.release_mask &= ~(UINT32_C(1) << pth.depth);
	p64_tfrwlock_release_rd(&lock->tfrwlock);
    }
}

void
p64_tfrwlock_r_acquire_wr(p64_tfrwlock_r_t *lock)
{
    if (UNLIKELY(pth.threadid == INVALID_TID))
    {
	pth.threadid = p64_gettid();
    }
    if (UNLIKELY(pth.depth == STACKSIZE))
    {
	fprintf(stderr, "tfrwlock_r: too many calls p64_tfrwlock_r_acquire_rd/wr\n");
	fflush(stderr);
	abort();
    }
    if (__atomic_load_n(&lock->owner, __ATOMIC_RELAXED) != pth.threadid)
    {
	if (UNLIKELY(find_lock(lock)))
	{
	    fprintf(stderr, "tfrwlock_r: acquire-write after acquire-read\n");
	    fflush(stderr);
	    abort();
	}
	uint16_t tkt;
	p64_tfrwlock_acquire_wr(&lock->tfrwlock, &tkt);
	__atomic_store_n(&lock->owner, pth.threadid, __ATOMIC_RELAXED);
	//First time this specific lock is acquired so it must be released later
	pth.release_mask |= UINT32_C(1) << pth.depth;
	pth.tkts[pth.depth] = tkt;
    }
    pth.stack[pth.depth++] = lock;
}

void
p64_tfrwlock_r_release_wr(p64_tfrwlock_r_t *lock)
{
    if (UNLIKELY(pth.depth == 0))
    {
	fprintf(stderr, "tfrwlock_r: superfluous call to p64_tfrwlock_r_release_wr()\n");
	fflush(stderr);
	abort();
    }
    else if (UNLIKELY(pth.stack[pth.depth - 1] != lock))
    {
	fprintf(stderr, "tfrwlock_r: p64_tfrwlock_r_release_wr() called for wrong lock\n");
	fflush(stderr);
	abort();
    }
    pth.depth--;
    if (pth.release_mask & (UINT32_C(1) << pth.depth))
    {
	pth.release_mask &= ~(UINT32_C(1) << pth.depth);
	__atomic_store_n(&lock->owner, INVALID_TID, __ATOMIC_RELAXED);
	p64_tfrwlock_release_wr(&lock->tfrwlock, pth.tkts[pth.depth]);
    }
}
