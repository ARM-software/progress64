//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "p64_rwsync_r.h"

#include "common.h"
#include "os_abstraction.h"

static __thread uint64_t threadid = INVALID_TID;

void
p64_rwsync_r_init(p64_rwsync_r_t *sync)
{
    p64_rwsync_init(&sync->rwsync);
    sync->owner = INVALID_TID;
    sync->count = 0;
}

p64_rwsync_t
p64_rwsync_r_acquire_rd(const p64_rwsync_r_t *sync)
{
    if (UNLIKELY(threadid == INVALID_TID))
    {
	threadid = p64_gettid();
    }

    //Check if we already have acquired the synchroniser for write
    //If so, we are in the middle of our own update and cannot wait
    //for this update to complete
    if (__atomic_load_n(&sync->owner, __ATOMIC_RELAXED) == threadid)
    {
	fprintf(stderr, "rwsync_r: acquire-read after acquire-write\n");
	fflush(stderr);
	abort();
    }
    return p64_rwsync_acquire_rd(&sync->rwsync);
}

bool
p64_rwsync_r_release_rd(const p64_rwsync_r_t *sync, p64_rwsync_t prv)
{
    return p64_rwsync_release_rd(&sync->rwsync, prv);
}

void
p64_rwsync_r_acquire_wr(p64_rwsync_r_t *sync)
{
    if (UNLIKELY(threadid == INVALID_TID))
    {
	threadid = p64_gettid();
    }

    if (__atomic_load_n(&sync->owner, __ATOMIC_RELAXED) != threadid)
    {
	p64_rwsync_acquire_wr(&sync->rwsync);
	assert(sync->count == 0);
	__atomic_store_n(&sync->owner, threadid, __ATOMIC_RELAXED);
    }
    sync->count++;
}

void
p64_rwsync_r_release_wr(p64_rwsync_r_t *sync)
{
    int32_t count = --(sync->count);
    if (count == 0)
    {
	__atomic_store_n(&sync->owner, INVALID_TID, __ATOMIC_RELAXED);
	p64_rwsync_release_wr(&sync->rwsync);
    }
    else if (UNLIKELY(count < 0))
    {
	fprintf(stderr, "rwsync_r: mismatched call to p64_rwsync_r_release_wr()\n");
	fflush(stderr);
	abort();
    }
}
