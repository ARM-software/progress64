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

void
p64_rwlock_r_init(p64_rwlock_r_t *lock)
{
    p64_rwlock_init(&lock->rwlock);
    lock->owner = P64_RWLOCK_INVALID_TID;
}

static __thread uint32_t rwl_count = 0;

void
p64_rwlock_r_acquire_rd(p64_rwlock_r_t *lock, int32_t tid)
{
    //Check if we already have acquired the lock for write
    //If so, we are in the middle of our own update and cannot wait
    //for this update to complete
    if (UNLIKELY(__atomic_load_n(&lock->owner, __ATOMIC_RELAXED) == tid))
    {
	fprintf(stderr, "rwlock_r: acquire-read after acquire-write\n");
	fflush(stderr);
	abort();
    }
    if (rwl_count == 0)
    {
	p64_rwlock_acquire_rd(&lock->rwlock);
    }
    rwl_count++;
}

void
p64_rwlock_r_release_rd(p64_rwlock_r_t *lock)
{
    if (UNLIKELY(rwl_count == 0))
    {
	fprintf(stderr, "rwlock_r: mismatched call to p64_rwlock_r_release_rd()\n");
	fflush(stderr);
	abort();
    }
    else if (--rwl_count == 0)
    {
	p64_rwlock_release_rd(&lock->rwlock);
    }
}

void
p64_rwlock_r_acquire_wr(p64_rwlock_r_t *lock, int32_t tid)
{
    if (UNLIKELY(tid == P64_RWLOCK_INVALID_TID))
    {
	fprintf(stderr, "rwlock_r: Invalid TID %d\n", tid);
	fflush(stderr);
	abort();
    }
    if (__atomic_load_n(&lock->owner, __ATOMIC_RELAXED) != tid)
    {
	p64_rwlock_acquire_wr(&lock->rwlock);
	assert(rwl_count == 0);
	__atomic_store_n(&lock->owner, tid, __ATOMIC_RELAXED);
    }
    rwl_count++;
}

void
p64_rwlock_r_release_wr(p64_rwlock_r_t *lock)
{
    if (UNLIKELY(rwl_count == 0))
    {
	fprintf(stderr, "rwlock_r: mismatched call to p64_rwlock_r_release_wr()\n");
	fflush(stderr);
	abort();
    }
    else if (--rwl_count == 0)
    {
	__atomic_store_n(&lock->owner, P64_RWLOCK_INVALID_TID, __ATOMIC_RELAXED);
	p64_rwlock_release_wr(&lock->rwlock);
    }
}
