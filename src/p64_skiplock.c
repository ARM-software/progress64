//Copyright (c) 2020, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause
//

#include <assert.h>
#include <stdbool.h>

#include "p64_skiplock.h"
#include "build_config.h"

#include "arch.h"
#include "lockfree.h"
#include "common.h"
#include "err_hnd.h"

#if __SIZEOF_POINTER__ == 8 && __SIZEOF_INT128__ == 16
#define ONE (unsigned __int128)1
#define BITS 96
#else
#define ONE (uint32_t)1
#define BITS 32
#endif

void
p64_skiplock_init(p64_skiplock_t *sl)
{
    sl->cur = 0;
    sl->mask = 0;
}

void
p64_skiplock_acquire(p64_skiplock_t *sl, uint32_t tkt)
{
    wait_until_equal(&sl->cur, tkt, __ATOMIC_ACQUIRE);
}

#if __SIZEOF_POINTER__ == 8 && __SIZEOF_INT128__ == 16
static inline uint32_t
ctz(unsigned __int128 x)
{
    if ((uint64_t)x != 0)
    {
	return __builtin_ctzll((uint64_t)x);
    }
    else
    {
	return 64 + __builtin_ctzll((uint64_t)(x >> 64));
    }
}
#else
static inline uint32_t
ctz(uint32_t x)
{
    return __builtin_ctz(x);
}
#endif

void
p64_skiplock_release(p64_skiplock_t *sl, uint32_t tkt)
{
    union
    {
	p64_skiplock_t sl;
	ptrpair_t pp;
    } old, new;
    do
    {
	old.sl = *sl;//Non-atomic read
	if (UNLIKELY(tkt != old.sl.cur))
	{
	    report_error("skiplock", "invalid ticket", tkt);
	    return;
	}
	//Always advance current ticket by one
	uint32_t advance = 1;
	//Also advance past any adjacent abandoned tickets
	advance += ctz(~old.sl.mask);
	assert(advance <= BITS + 1);
	new.sl.cur = old.sl.cur + advance;
	if (advance < BITS)
	{
	    new.sl.mask = old.sl.mask >> advance;
	}
	else
	{
	    new.sl.mask = 0;
	}
    }
    while (!lockfree_compare_exchange_pp_frail((ptrpair_t *)sl,
					       &old.pp,
					       new.pp,
					       /*weak=*/true,
					       __ATOMIC_RELEASE,
					       __ATOMIC_RELAXED));
}

void
p64_skiplock_skip(p64_skiplock_t *sl, uint32_t tkt)
{
    //Wait until tkt in range
    while (UNLIKELY(tkt - __atomic_load_n(&sl->cur, __ATOMIC_RELAXED) > BITS))
    {
	//TODO use WFE
	doze();
    }
    //Now (tkt - sl->cur) <= BITS
    union
    {
	p64_skiplock_t sl;
	ptrpair_t pp;
    } old, new;
    do
    {
	old.sl = *sl;//Non-atomic read
	uint32_t dif = tkt - old.sl.cur;
	assert(dif <= BITS);
	if (UNLIKELY(dif == 0))
	{
	    //Our turn already
	    p64_skiplock_release(sl, tkt);
	    return;
	}
	uint32_t ourbit = dif - 1;
	//Our bit must not already be set
	assert((old.sl.mask & (ONE << ourbit)) == 0);
	new = old;
	new.sl.mask |= ONE << ourbit;
    }
    while (UNLIKELY(!lockfree_compare_exchange_pp_frail((ptrpair_t *)sl,
						        &old.pp,
						        new.pp,
						        /*weak=*/true,
						        __ATOMIC_RELAXED,
						        __ATOMIC_RELAXED)));
}
