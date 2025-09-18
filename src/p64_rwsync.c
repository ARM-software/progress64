//Copyright (c) 2017,2025, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "p64_rwsync.h"
#include "build_config.h"

#include "common.h"
#include "atomic.h"
#include "err_hnd.h"

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
    if (UNLIKELY(((l = atomic_load_n(sync, mo)) & RWSYNC_WRITER) != 0))
    {
	while (((l = atomic_ldx(sync, mo)) & RWSYNC_WRITER) != 0)
	{
	    WFE();
	    //When verifying, we need to force a thread yield in this poll loop
	    VERIFY_YIELD();

	}
    }
    assert((l & RWSYNC_WRITER) == 0);//No writer in progress
    return l;
}

p64_rwsync_t
p64_rwsync_acquire_rd(const p64_rwsync_t *sync)
{
    //Wait for any present writer to go away
    //A0: read sync, synchronize with A1
    return wait_for_no_writer(sync, __ATOMIC_ACQUIRE);
}

bool
p64_rwsync_release_rd(const p64_rwsync_t *sync, p64_rwsync_t prv)
{
    //B0: load-relaxed(data) (by caller) + fence-acquire, synchronize with B1
    atomic_thread_fence(__ATOMIC_ACQUIRE);
    //Test if sync is unchanged => success
    return atomic_load_n(sync, __ATOMIC_RELAXED) == prv;
}

void
p64_rwsync_acquire_wr(p64_rwsync_t *sync)
{
    p64_rwsync_t l;
    for (;;)
    {
	//Set writer flag (it may already be set)
	//A2: read (and write) sync, synchronize with A1
	l = atomic_fetch_or(sync, RWSYNC_WRITER, __ATOMIC_ACQUIRE);
	if ((l & RWSYNC_WRITER) == 0)
	{
	    //Writer flag was clear => now we own rwsync
	    break;
	}
	//Wait for any present writer to go away
	l = wait_for_no_writer(sync, __ATOMIC_RELAXED);
    }
    //B1: fence-release + store-relaxed(data) (by caller), synchronize with B0
    atomic_thread_fence(__ATOMIC_RELEASE);
}

void
p64_rwsync_release_wr(p64_rwsync_t *sync)
{
    p64_rwsync_t cur = *sync;
    if (UNLIKELY(cur & RWSYNC_WRITER) == 0)
    {
	report_error("rwsync", "invalid write release", sync);
	return;
    }

    //Increment, clearing writer flag
    //A1: write sync, synchronize with A0/A2
    atomic_store_n(sync, cur + 1, __ATOMIC_RELEASE);
}

#define ATOMIC_COPY(_d, _s, _sz, _type) \
({ \
    _type val = atomic_load_n((const _type *)(_s), __ATOMIC_RELAXED); \
    _s += sizeof(_type); \
    atomic_store_n((_type *)(_d), val, __ATOMIC_RELAXED); \
    _d += sizeof(_type); \
    _sz -= sizeof(_type); \
})

static void
atomic_memcpy(char *dst, const char *src, size_t sz)
{
#if __SIZEOF_POINTER__ == 8
    while (sz >= sizeof(uint64_t))
	ATOMIC_COPY(dst, src, sz, uint64_t);
    if (sz >= sizeof(uint32_t))
	ATOMIC_COPY(dst, src, sz, uint32_t);
#else //__SIZEOF_POINTER__ == 4
    while (sz >= sizeof(uint32_t))
	ATOMIC_COPY(dst, src, sz, uint32_t);
#endif
    if (sz >= sizeof(uint16_t))
	ATOMIC_COPY(dst, src, sz, uint16_t);
    if (sz >= sizeof(uint8_t))
	ATOMIC_COPY(dst, src, sz, uint8_t);
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
	atomic_memcpy(dst, data, len);
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
    atomic_memcpy(data, src, len);
    p64_rwsync_release_wr(sync);
}
