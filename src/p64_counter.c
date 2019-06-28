//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "p64_counter.h"
#include "p64_hazardptr.h"
#include "build_config.h"
#include "os_abstraction.h"

#include "common.h"
#include "arch.h"
#include "thr_idx.h"

static void
eprintf_invalid_counter(p64_counter_t cntid)
{
    fprintf(stderr, "counter: Invalid counter %u\n", cntid);
    fflush(stderr);
    abort();
}

static void
eprintf_thread_not_registered(void)
{
    fprintf(stderr, "counter: Thread not registered\n");
    fflush(stderr);
    abort();
}

struct p64_cntdomain
{
    uint32_t ncounters;
    uint64_t *shared;
    uint64_t *perthread[MAXTHREADS];
    uint64_t free[];//Bitmask of free counters
};

#define BITSPERWORD 64

p64_cntdomain_t *
p64_cntdomain_alloc(uint32_t ncounters)
{
    ncounters++;//Allow for null element (cntid=0)
    uint32_t nwords = (ncounters + BITSPERWORD - 1) / BITSPERWORD;
    size_t nbytes = sizeof(p64_cntdomain_t) +
		    (nwords + ncounters) * sizeof(uint64_t);
    p64_cntdomain_t *cntd = p64_malloc(nbytes, CACHE_LINE);
    if (cntd != NULL)
    {
	//Clear everything including shared counters
	memset(cntd, 0, nbytes);
	cntd->ncounters = ncounters;
	cntd->shared = &cntd->free[nwords];
	for (uint32_t t = 0; t < MAXTHREADS; t++)
	{
	    cntd->perthread[t] = NULL;
	}
	//Mark all counters as free
	for (uint32_t b = 0; b < ncounters;)
	{
	    if (b + BITSPERWORD <= ncounters)
	    {
		cntd->free[b / BITSPERWORD] = ~UINT64_C(0);
		b += BITSPERWORD;
	    }
	    else
	    {
		cntd->free[b / BITSPERWORD] |= UINT64_C(1) << (b % BITSPERWORD);
		b++;
	    }
	}
	//Reserve counter 0 by marking it as used
	cntd->free[0] &= ~UINT64_C(1);
	return cntd;
    }
    return NULL;
}

void
p64_cntdomain_free(p64_cntdomain_t *cntd)
{
    for (uint32_t i = 0; i < MAXTHREADS; i++)
    {
	if (__atomic_load_n(&cntd->perthread[i], __ATOMIC_RELAXED) != NULL)
	{
	    fprintf(stderr, "counter: Registered threads still present\n");
	    fflush(stderr);
	    abort();
	}
    }
    p64_mfree(cntd);
}

static __thread struct
{
    int32_t tidx;
    uint32_t count;
} pth = { -1, 0 };

void
p64_cntdomain_register(p64_cntdomain_t *cntd)
{
    if (UNLIKELY(pth.count++ == 0))
    {
	pth.tidx = p64_idx_alloc();
    }
    if (UNLIKELY(cntd->perthread[pth.tidx] != NULL))
    {
	eprintf_thread_not_registered();
    }
    size_t sz = cntd->ncounters * sizeof(uint64_t);
    uint64_t *counters = p64_malloc(sz, 0);
    if (counters == NULL)
    {
	fprintf(stderr, "counter: Failed to allocate private stash\n");
	fflush(stderr);
	abort();
    }
    //Initialise all private counters
    memset(counters, 0, sz);
    //Publish private counters
    __atomic_store_n(&cntd->perthread[pth.tidx], counters, __ATOMIC_RELEASE);
}

void
p64_cntdomain_unregister(p64_cntdomain_t *cntd)
{
    if (UNLIKELY(pth.count == 0))
    {
	eprintf_thread_not_registered();
    }
    uint64_t *counters = cntd->perthread[pth.tidx];
    if (UNLIKELY(counters == NULL))
    {
	eprintf_thread_not_registered();
    }
    //Move all counters from private to shared locations
    for (uint32_t i = 0; i < cntd->ncounters; i++)
    {
	uint32_t val = counters[i];
	if (val != 0)
	{
	    //Move counter value from private to shared location
	    //This is not atomic!
	    __atomic_store_n(&counters[i], 0, __ATOMIC_RELAXED);
	    __atomic_fetch_add(&cntd->shared[i], val, __ATOMIC_RELAXED);
	}
    }
    //Unpublish private counters
    __atomic_store_n(&cntd->perthread[pth.tidx], NULL, __ATOMIC_RELEASE);
    //Retire counter array
    while (!p64_hazptr_retire(counters, p64_mfree))
    {
	doze();
    }
    //Decrement refcnt and conditionally release our thread index
    if (--pth.count == 0)
    {
	p64_idx_free(pth.tidx);
	pth.tidx = -1;
    }
}

p64_counter_t
p64_counter_alloc(p64_cntdomain_t *cntd)
{
    uint32_t nwords = (cntd->ncounters + BITSPERWORD - 1) / BITSPERWORD;
    for (uint32_t i = 0; i < nwords; i++)
    {
	uint64_t w = __atomic_load_n(&cntd->free[i], __ATOMIC_RELAXED);
	while (w != 0)
	{
	    uint32_t b = __builtin_ctzl(w);
	    //Attempt to clear free bit
	    if (__atomic_compare_exchange_n(&cntd->free[i],
					    &w,
					    w & ~(UINT64_C(1) << b),
					    /*weak*/0,
					    __ATOMIC_ACQUIRE,
					    __ATOMIC_RELAXED))
	    {
		//Success, counter allocated
		uint32_t cntid = i * BITSPERWORD + b;
		cntd->shared[cntid] = 0;
		return cntid;
	    }
	}
    }
    return P64_COUNTER_INVALID;
}

void
p64_counter_free(p64_cntdomain_t *cntd, p64_counter_t cntid)
{
    if (UNLIKELY(cntid == P64_COUNTER_INVALID ||
		 cntid >= cntd->ncounters))
    {
	eprintf_invalid_counter(cntid);
    }
    //Check that bit is not already set (counter is free)
    if (cntd->free[cntid / BITSPERWORD] & (UINT64_C(1) << (cntid % BITSPERWORD)))
    {
	fprintf(stderr, "counter: Counter %u already free\n", cntid);
	fflush(stderr);
	abort();
    }
    //Set free bit and free counter
    __atomic_fetch_or(&cntd->free[cntid / BITSPERWORD],
		      UINT64_C(1) << (cntid % BITSPERWORD),
		      __ATOMIC_RELEASE);
}

void
p64_counter_add(p64_cntdomain_t *cntd, p64_counter_t cntid, uint64_t val)
{
    if (UNLIKELY(pth.count == 0))
    {
	eprintf_thread_not_registered();
    }
    if (UNLIKELY(cntid == P64_COUNTER_INVALID ||
		 cntid >= cntd->ncounters))
    {
	eprintf_invalid_counter(cntid);
    }
    uint64_t *counters = cntd->perthread[pth.tidx];
    uint64_t old = __atomic_load_n(&counters[cntid], __ATOMIC_RELAXED);
    __atomic_store_n(&counters[cntid], old + val, __ATOMIC_RELAXED);
}

uint64_t
p64_counter_read(p64_cntdomain_t *cntd, p64_counter_t cntid)
{
    if (UNLIKELY(cntid == P64_COUNTER_INVALID ||
		 cntid >= cntd->ncounters))
    {
	eprintf_invalid_counter(cntid);
    }
    p64_hazardptr_t hp = P64_HAZARDPTR_NULL;
    uint64_t sum = cntd->shared[cntid];
    //Add values from private (per thread) locations
    for (uint32_t t = 0; t < MAXTHREADS; t++)
    {
	uint64_t *counters = p64_hazptr_acquire(&cntd->perthread[t], &hp);
	if (counters != NULL)
	{
	    sum += __atomic_load_n(&counters[cntid], __ATOMIC_RELAXED);
	}
    }
    p64_hazptr_release(&hp);
    return sum;
}

void
p64_counter_reset(p64_cntdomain_t *cntd, p64_counter_t cntid)
{
    if (UNLIKELY(cntid == P64_COUNTER_INVALID ||
		 cntid >= cntd->ncounters))
    {
	eprintf_invalid_counter(cntid);
    }
    uint64_t cur = p64_counter_read(cntd, cntid);
    __atomic_fetch_sub(&cntd->shared[cntid], cur, __ATOMIC_RELAXED);
}
