//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "p64_qsbr.h"
#include "build_config.h"

#include "arch.h"
#include "lockfree.h"
#include "common.h"
#include "thr_idx.h"

struct p64_qsbr
{
    uint64_t cur_int;//Current interval
    uint32_t nelems;
    uint32_t high_wm;//High watermark of thread index
    uint64_t last_int[MAXTHREADS] ALIGNED(CACHE_LINE);//Each thread's last quiescent interval
};

//Value larger than all possible intervals
#define INFINITE (~(uint64_t)0)

p64_qsbr_t *
p64_qsbr_alloc(uint32_t nelems)
{
    size_t nbytes = ROUNDUP(sizeof(p64_qsbr_t), CACHE_LINE);
    p64_qsbr_t *qsbr = aligned_alloc(CACHE_LINE, nbytes);
    if (qsbr != NULL)
    {
	qsbr->cur_int = 0;
	qsbr->nelems = nelems;
	qsbr->high_wm = 0;
	for (uint32_t i = 0; i < MAXTHREADS; i++)
	{
	    qsbr->last_int[i] = INFINITE;
	}
	return qsbr;
    }
    return NULL;
}

static uint64_t
find_min(const uint64_t vec[], uint32_t num)
{
    uint64_t min = INFINITE;
    for (uint32_t i = 0; i < num; i++)
    {
	uint64_t t = __atomic_load_n(&vec[i], __ATOMIC_ACQUIRE);
	if (t < min)
	{
	    min = t;
	}
    }
    return min;
}

void
p64_qsbr_free(p64_qsbr_t *qsbr)
{
    uint64_t last_int = find_min(qsbr->last_int, qsbr->high_wm);
    if (last_int < __atomic_load_n(&qsbr->cur_int, __ATOMIC_RELAXED))
    {
	fprintf(stderr, "qsbr: Some threads may still have references\n");
	abort();
    }
    free(qsbr);
}

typedef void *userptr_t;

struct element
{
    userptr_t ptr;
    void (*cb)(userptr_t);
    uint64_t interval;
};

struct thread_state
{
    p64_qsbr_t *qsbr;
    uint64_t last_int;//Last seen interval
    uint32_t idx;//Thread index
    //Removed but not yet recycled objects
    uint32_t nitems;
    uint32_t maxitems;
    struct element objs[];
} ALIGNED(CACHE_LINE);

static __thread struct thread_state *TS = NULL;

static struct thread_state *
alloc_ts(p64_qsbr_t *qsbr)
{
    assert(TS == NULL);
    size_t nbytes = ROUNDUP(sizeof(struct thread_state) +
			    qsbr->nelems * sizeof(struct element), CACHE_LINE);
    struct thread_state *ts = aligned_alloc(CACHE_LINE, nbytes);
    if (ts == NULL)
    {
	perror("malloc"), exit(EXIT_FAILURE);
    }
    //Attempt to allocate a thread index
    int32_t idx = p64_idx_alloc();
    if (idx < 0)
    {
	fprintf(stderr, "Too many threads using QBSR\n"), abort();
    }
    //Conditionally update high watermark of indexes
    lockfree_fetch_umax_4(&qsbr->high_wm, (uint32_t)idx + 1, __ATOMIC_RELAXED);
    ts->qsbr = qsbr;
    ts->last_int = INFINITE;
    ts->idx = idx;
    ts->nitems = 0;
    ts->maxitems = qsbr->nelems;
    __atomic_store_n(&qsbr->last_int[idx], ts->last_int, __ATOMIC_RELAXED);
    return ts;
}

void
p64_qsbr_acquire(p64_qsbr_t *qsbr)
{
    if (UNLIKELY(TS == NULL))
    {
	TS = alloc_ts(qsbr);
    }
    uint64_t last_int = __atomic_load_n(&qsbr->cur_int, __ATOMIC_RELAXED);
    __atomic_store_n(&qsbr->last_int[TS->idx], last_int, __ATOMIC_RELAXED);
    TS->last_int = last_int;
    smp_fence(StoreLoad);
}

void
p64_qsbr_quiescent(void)
{
    if (UNLIKELY(TS == NULL))
    {
	fprintf(stderr, "qsbr: p64_qsbr_acquire() not called\n"), abort();
    }
    p64_qsbr_t *qsbr = TS->qsbr;
    uint64_t last_int = __atomic_load_n(&qsbr->cur_int, __ATOMIC_RELAXED);
    if (last_int != TS->last_int)
    {
	//Release order to contain all our previous access to shared objects
	__atomic_store_n(&qsbr->last_int[TS->idx], last_int, __ATOMIC_RELEASE);
	TS->last_int = last_int;
    }
    smp_fence(StoreLoad);//TODO move up?
}

void
p64_qsbr_release(void)
{
    if (UNLIKELY(TS == NULL))
    {
	fprintf(stderr, "qsbr: p64_qsbr_acquire() not called\n"), abort();
    }
    p64_qsbr_t *qsbr = TS->qsbr;
    //Mark thread as inactive, no references kept
    __atomic_store_n(&qsbr->last_int[TS->idx], INFINITE, __ATOMIC_RELEASE);
    TS->last_int = INFINITE;
}

void
p64_qsbr_cleanup(void)
{
    if (UNLIKELY(TS == NULL))
    {
	return;
    }
    if (TS->nitems != 0)
    {
	fprintf(stderr, "qsbr: Thread index %d has %u unreclaimed objects\n",
		TS->idx, TS->nitems);
	abort();
    }
    p64_idx_free(TS->idx);
    free(TS);
    TS = NULL;
}

uint32_t
p64_qsbr_reclaim(void)
{
    if (UNLIKELY(TS == NULL))
    {
	return 0;
    }
    if (TS->nitems == 0)
    {
	//Nothing to reclaim
	return 0;
    }
    p64_qsbr_t *qsbr = TS->qsbr;
    uint64_t last_int = find_min(qsbr->last_int, qsbr->high_wm);
    uint32_t nreclaimed = 0;
    //Traverse list of retired objects
    uint32_t nitems = 0;
    for (uint32_t i = 0; i < TS->nitems; i++)
    {
	struct element elem = TS->objs[i];
	if (last_int > elem.interval)
	{
	    //All threads have observed a later interval =>
	    //No thread has any reference to this object, reclaim it
	    elem.cb(elem.ptr);
	    nreclaimed++;
	}
	else
	{
	    //Retired object still referenced, keep it in rlist
	    TS->objs[nitems++] = elem;
	}
    }
    //Some objects may remain in the list of retired objects
    TS->nitems = nitems;
    //Return number of remaining unreclaimed objects
    //Caller can compute number of available slots
    return nitems;
}

bool
p64_qsbr_retire(void *ptr,
		void (*cb)(void *ptr))
{
    if (UNLIKELY(TS == NULL))
    {
	fprintf(stderr, "qsbr: p64_qsbr_acquire() not called\n"), abort();
    }
    p64_qsbr_t *qsbr = TS->qsbr;
    if (TS->nitems == TS->maxitems)
    {
	if (p64_qsbr_reclaim() == TS->maxitems)
	{
	    return false;//No space for object
	}
    }
    assert(TS->nitems < TS->maxitems);
    uint32_t i = TS->nitems++;
    TS->objs[i].ptr = ptr;
    TS->objs[i].cb = cb;
    //Create a new interval
    //Retired object belongs to previous interval
    //Release order to ensure removal is visible before new interval
    TS->objs[i].interval = __atomic_fetch_add(&qsbr->cur_int, 1, __ATOMIC_RELEASE);
    //When all threads have observed the new interval, this object can be reclaimed
    return true;
}
