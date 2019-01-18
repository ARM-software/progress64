//Copyright (c) 2017, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "p64_rwsync.h"
#include "build_config.h"

#include "common.h"
#include "arch.h"

#define RWSYNC_WRITER 1U

void
p64_rwsync_init(p64_rwsync_t *sync)
{
    *sync = 0;
}

static inline p64_rwsync_t
wait_for_no_writer(const p64_rwsync_t *sync, int mo)
{
    p64_rwsync_t l;
    if (((l = __atomic_load_n(sync, mo)) & RWSYNC_WRITER) != 0)
    {
	SEVL();
	while (WFE() &&
	       ((l = LDXR32(sync, mo)) & RWSYNC_WRITER) != 0)
	{
	    DOZE();
	}
    }
    assert((l & RWSYNC_WRITER) == 0);//No writer in progress
    return l;
}

p64_rwsync_t
p64_rwsync_acquire_rd(const p64_rwsync_t *sync)
{
    //Wait for any present writer to go away
    return wait_for_no_writer(sync, __ATOMIC_ACQUIRE);
}

bool
p64_rwsync_release_rd(const p64_rwsync_t *sync, p64_rwsync_t prv)
{
    smp_fence(LoadLoad);//Load-only barrier due to reader-sync
    //Test if sync is unchanged => success
    return __atomic_load_n(sync, __ATOMIC_RELAXED) == prv;
}

void
p64_rwsync_acquire_wr(p64_rwsync_t *sync)
{
    p64_rwsync_t l;
    do
    {
	//Wait for any present writer to go away
	l = wait_for_no_writer(sync, __ATOMIC_RELAXED);
	//Attempt to increment, setting writer flag
    }
    while (!__atomic_compare_exchange_n(sync, &l, l + RWSYNC_WRITER,
					/*weak=*/true,
					__ATOMIC_ACQUIRE, __ATOMIC_RELAXED));
}

void
p64_rwsync_release_wr(p64_rwsync_t *sync)
{
    p64_rwsync_t cur = *sync;
    if (UNLIKELY(cur & RWSYNC_WRITER) == 0)
    {
	fprintf(stderr, "Invalid write release of RW sync %p\n", sync), abort();
    }

    //Increment, clearing writer flag
#ifdef USE_DMB
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(sync, cur + 1, __ATOMIC_RELAXED);
#else
    __atomic_store_n(sync, cur + 1, __ATOMIC_RELEASE);
#endif
}

void
p64_rwsync_read(p64_rwsync_t *sync,
		void *dst,
		const void *data,
		size_t len)
{
    p64_rwsync_t prv;
    do
    {
	prv = p64_rwsync_acquire_rd(sync);
	memcpy(dst, data, len);
    }
    while (!p64_rwsync_release_rd(sync, prv));
}

void
p64_rwsync_write(p64_rwsync_t *sync,
		 const void *src,
		 void *data,
		 size_t len)
{
    p64_rwsync_acquire_wr(sync);
    memcpy(data, src, len);
    p64_rwsync_release_wr(sync);
}
