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

static void
eprintf_not_registered(void)
{
    fprintf(stderr, "qsbr: p64_qsbr_register() not called\n");
    fflush(stderr);
}

struct p64_qsbrdomain
{
    uint64_t current;//Current interval
    uint32_t nelems;
    uint32_t high_wm;//High watermark of thread index
    uint64_t intervals[MAXTHREADS] ALIGNED(CACHE_LINE);//Each thread's last quiescent interval
};

//Value larger than all possible intervals
#define INFINITE (~(uint64_t)0)

p64_qsbrdomain_t *
p64_qsbr_alloc(uint32_t nelems)
{
    size_t nbytes = ROUNDUP(sizeof(p64_qsbrdomain_t), CACHE_LINE);
    p64_qsbrdomain_t *qsbr = aligned_alloc(CACHE_LINE, nbytes);
    if (qsbr != NULL)
    {
	qsbr->current = 0;
	qsbr->nelems = nelems;
	qsbr->high_wm = 0;
	for (uint32_t i = 0; i < MAXTHREADS; i++)
	{
	    qsbr->intervals[i] = INFINITE;
	}
	return qsbr;
    }
    return NULL;
}

//Find the smallest value in the array
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
p64_qsbr_free(p64_qsbrdomain_t *qsbr)
{
    uint64_t interval = find_min(qsbr->intervals, qsbr->high_wm);
    if (interval != INFINITE)
    {
	fprintf(stderr, "qsbr: Registered threads still present\n");
	fflush(stderr);
	abort();
    }
    free(qsbr);
}

typedef void *userptr_t;

struct object
{
    userptr_t ptr;
    void (*cb)(userptr_t);
    uint64_t interval;
};

struct thread_state
{
    p64_qsbrdomain_t *qsbr;
    uint64_t interval;//Last seen interval
    uint32_t recur;//Acquire/release recursion level
    uint32_t idx;//Thread index
    //Removed but not yet reclaimed objects
    uint32_t nobjs;
    uint32_t maxobjs;
    struct object objs[];
} ALIGNED(CACHE_LINE);

static __thread struct thread_state *TS = NULL;

static struct thread_state *
alloc_ts(p64_qsbrdomain_t *qsbr)
{
    assert(TS == NULL);
    //Attempt to allocate a thread index
    int32_t idx = p64_idx_alloc();
    if (idx < 0)
    {
	fprintf(stderr, "qsbr: Too many registered threads\n");
	fflush(stderr);
	abort();
    }
    size_t nbytes = ROUNDUP(sizeof(struct thread_state) +
			    qsbr->nelems * sizeof(struct object), CACHE_LINE);
    struct thread_state *ts = aligned_alloc(CACHE_LINE, nbytes);
    if (ts == NULL)
    {
	perror("malloc"), exit(EXIT_FAILURE);
    }
    //Conditionally update high watermark of indexes
    lockfree_fetch_umax_4(&qsbr->high_wm, (uint32_t)idx + 1, __ATOMIC_RELAXED);
    ts->qsbr = qsbr;
    ts->interval = INFINITE;
    ts->recur = 0;
    ts->idx = idx;
    ts->nobjs = 0;
    ts->maxobjs = qsbr->nelems;
    assert(qsbr->intervals[idx] == INFINITE);
    return ts;
}

void
p64_qsbr_register(p64_qsbrdomain_t *qsbr)
{
    if (UNLIKELY(TS == NULL))
    {
	TS = alloc_ts(qsbr);
    }
    uint64_t interval = __atomic_load_n(&qsbr->current, __ATOMIC_RELAXED);
    __atomic_store_n(&qsbr->intervals[TS->idx], interval, __ATOMIC_RELAXED);
    TS->interval = interval;
    //Ensure our interval is observable before any reads are observed
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

void
p64_qsbr_unregister(void)
{
    if (UNLIKELY(TS == NULL))
    {
	return;
    }
    if (TS->nobjs != 0)
    {
	fprintf(stderr, "qsbr: Thread has %u unreclaimed objects\n", TS->nobjs);
	fflush(stderr);
	abort();
    }
    //Mark thread as inactive, no references kept
    __atomic_store_n(&TS->qsbr->intervals[TS->idx], INFINITE, __ATOMIC_RELEASE);
    p64_idx_free(TS->idx);
    free(TS);
    TS = NULL;
}

void
p64_qsbr_acquire(void)
{
    if (UNLIKELY(TS == NULL))
    {
	eprintf_not_registered(); abort();
    }
    TS->recur++;
}

static inline void
quiescent(void)
{
    p64_qsbrdomain_t *qsbr = TS->qsbr;
    uint64_t interval = __atomic_load_n(&qsbr->current, __ATOMIC_RELAXED);
    if (interval != TS->interval)
    {
	//Release order to contain all our previous access to shared objects
	__atomic_store_n(&qsbr->intervals[TS->idx], interval, __ATOMIC_RELEASE);
	TS->interval = interval;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
    }
}

void
p64_qsbr_quiescent(void)
{
    if (UNLIKELY(TS == NULL))
    {
	eprintf_not_registered(); abort();
    }
    quiescent();
}

void
p64_qsbr_release(void)
{
    if (UNLIKELY(TS == NULL))
    {
	eprintf_not_registered(); abort();
    }
    if (UNLIKELY(TS->recur == 0))
    {
	fprintf(stderr, "qsbr: mismatched call to p64_qsbr_release()\n");
	fflush(stderr);
	abort();
    }
    if (--TS->recur == 0)
    {
	quiescent();
    }
}

//Traverse all pending objects and reclaim those that have no references
static uint32_t
garbage_collect(void)
{
    p64_qsbrdomain_t *qsbr = TS->qsbr;
    uint64_t interval = find_min(qsbr->intervals, qsbr->high_wm);
    //Traverse list of pending objects
    uint32_t nobjs = 0;
    for (uint32_t i = 0; i < TS->nobjs; i++)
    {
	struct object obj = TS->objs[i];
	if (interval > obj.interval)
	{
	    //All threads have observed a later interval =>
	    //No thread has any reference to this object, reclaim it
	    obj.cb(obj.ptr);
	}
	else
	{
	    //Retired object still referenced, keep it in rlist
	    TS->objs[nobjs++] = obj;
	}
    }
    //Some objects may remain in the list of retired objects
    TS->nobjs = nobjs;
    //Return number of remaining unreclaimed objects
    //Caller can compute number of available slots
    return nobjs;
}

//Retire an object
//If necessary, perform garbage collection on retired objects
bool
p64_qsbr_retire(void *ptr,
		void (*cb)(void *ptr))
{
    if (UNLIKELY(TS == NULL))
    {
	fprintf(stderr, "qsbr: p64_qsbr_acquire() not called\n"), abort();
    }
    if (UNLIKELY(TS->nobjs == TS->maxobjs))
    {
	if (p64_qsbr_reclaim() == TS->maxobjs)
	{
	    return false;//No space for object
	}
    }
    assert(TS->nobjs < TS->maxobjs);
    uint32_t i = TS->nobjs++;
    TS->objs[i].ptr = ptr;
    TS->objs[i].cb = cb;
    //Create a new interval
    //Release order to ensure removal is observable before new interval is
    //created and can be observed
    uint32_t new_interval = __atomic_fetch_add(&TS->qsbr->current,
					       1,
					       __ATOMIC_RELEASE);
    //Retired object belongs to previous interval
    TS->objs[i].interval = new_interval;
    //The object can be reclaimed when all threads have observed
    //the new interval
    return true;
}

uint32_t
p64_qsbr_reclaim(void)
{
    if (UNLIKELY(TS == NULL))
    {
	eprintf_not_registered(); abort();
    }
    if (TS->nobjs == 0)
    {
	//Nothing to reclaim
	return 0;
    }
    //Try to reclaim objects
    uint32_t nremaining = garbage_collect();
    return nremaining;
}
