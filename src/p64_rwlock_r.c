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

void
p64_rwlock_r_init(p64_rwlock_r_t *lock)
{
    p64_rwlock_init(&lock->rwlock);
    lock->owner = INVALID_TID;
}

static __thread struct
{
    uint64_t threadid;
    uint32_t rwl_count;
} pth = { INVALID_TID, 0 };

void
p64_rwlock_r_acquire_rd(p64_rwlock_r_t *lock)
{
    if (UNLIKELY(pth.threadid == INVALID_TID))
    {
	pth.threadid = p64_gettid();
    }
    //Check if we already have acquired the lock for write
    //If so, we are in the middle of our own update and cannot wait
    //for this update to complete
    if (UNLIKELY(__atomic_load_n(&lock->owner, __ATOMIC_RELAXED) == pth.threadid))
    {
	fprintf(stderr, "rwlock_r: acquire-read after acquire-write\n");
	fflush(stderr);
	abort();
    }
    if (pth.rwl_count == 0)
    {
	p64_rwlock_acquire_rd(&lock->rwlock);
    }
    pth.rwl_count++;
}

void
p64_rwlock_r_release_rd(p64_rwlock_r_t *lock)
{
    if (UNLIKELY(pth.rwl_count == 0))
    {
	fprintf(stderr, "rwlock_r: mismatched call to p64_rwlock_r_release_rd()\n");
	fflush(stderr);
	abort();
    }
    else if (--pth.rwl_count == 0)
    {
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
    if (__atomic_load_n(&lock->owner, __ATOMIC_RELAXED) != pth.threadid)
    {
	p64_rwlock_acquire_wr(&lock->rwlock);
	assert(pth.rwl_count == 0);
	__atomic_store_n(&lock->owner, pth.threadid, __ATOMIC_RELAXED);
    }
    pth.rwl_count++;
}

void
p64_rwlock_r_release_wr(p64_rwlock_r_t *lock)
{
    if (UNLIKELY(pth.rwl_count == 0))
    {
	fprintf(stderr, "rwlock_r: mismatched call to p64_rwlock_r_release_wr()\n");
	fflush(stderr);
	abort();
    }
    else if (--pth.rwl_count == 0)
    {
	__atomic_store_n(&lock->owner, INVALID_TID, __ATOMIC_RELAXED);
	p64_rwlock_release_wr(&lock->rwlock);
    }
}
