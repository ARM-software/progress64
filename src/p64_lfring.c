//Copyright (c) 2018-2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include "p64_lfring.h"
#include "build_config.h"
#include "os_abstraction.h"

#include "arch.h"
#include "lockfree.h"
#include "common.h"
#include "err_hnd.h"

#define SUPPORTED_FLAGS (P64_LFRING_F_SPENQ | P64_LFRING_F_MPENQ | \
			 P64_LFRING_F_SCDEQ | P64_LFRING_F_MCDEQ)

typedef uintptr_t ringidx_t;
struct element
{
    void *ptr;
    uintptr_t idx;
};

struct p64_lfring
{
    ringidx_t head;
#ifdef USE_SPLIT_PRODCONS
    ringidx_t tail ALIGNED(CACHE_LINE);
#else
    ringidx_t tail;
#endif
    uint32_t mask;
    uint32_t flags;
    struct element ring[] ALIGNED(CACHE_LINE);
} ALIGNED(CACHE_LINE);

p64_lfring_t *
p64_lfring_alloc(uint32_t nelems, uint32_t flags)
{
    unsigned long ringsz = ROUNDUP_POW2(nelems);
    if (nelems == 0 || ringsz == 0 || ringsz > 0x80000000)
    {
	report_error("lfring", "invalid number of elements", nelems);
	return NULL;
    }
    if ((flags & ~SUPPORTED_FLAGS) != 0)
    {
	report_error("lfring", "invalid flags", flags);
	return NULL;
    }

    size_t nbytes = sizeof(p64_lfring_t) + ringsz * sizeof(struct element);
    p64_lfring_t *lfr = p64_malloc(nbytes, CACHE_LINE);
    if (lfr != NULL)
    {
	lfr->head = 0;
	lfr->tail = 0;
	lfr->mask = ringsz - 1;
	lfr->flags = flags;
	for (ringidx_t i = 0; i < ringsz; i++)
	{
	    lfr->ring[i].ptr = NULL;
	    lfr->ring[i].idx = i - ringsz;
	}
	return lfr;
    }
    return NULL;
}

void
p64_lfring_free(p64_lfring_t *lfr)
{
    if (lfr != NULL)
    {
	if (lfr->head != lfr->tail)
	{
	    report_error("lfring", "ring buffer not empty", lfr);
	    return;
	}
	p64_mfree(lfr);
    }
}

//True if 'a' is before 'b' ('a' < 'b') in serial number arithmetic
static inline bool
before(ringidx_t a, ringidx_t b)
{
    return (intptr_t)(a - b) < 0;
}

static inline ringidx_t
cond_update(ringidx_t *loc, ringidx_t neu)
{
    ringidx_t old = __atomic_load_n(loc, __ATOMIC_RELAXED);
    do
    {
	if (before(neu, old))//neu < old
	{
	    return old;
	}
	//Else neu > old, need to update *loc
    }
    while (!__atomic_compare_exchange_n(loc,
					&old,//Updated on failure
					neu,
					/*weak=*/true,
					__ATOMIC_RELEASE,
					__ATOMIC_RELAXED));
    return neu;
}

static inline ringidx_t
cond_reload(ringidx_t idx, const ringidx_t *loc)
{
    ringidx_t fresh = __atomic_load_n(loc, __ATOMIC_RELAXED);
    if (before(idx, fresh))
    {
	//fresh is after idx, use this instead
	idx = fresh;
    }
    else
    {
	//Continue with next slot
	idx++;
    }
    return idx;
}

//Enqueue elements at tail
uint32_t
p64_lfring_enqueue(p64_lfring_t *lfr,
		   void *const *restrict elems,
		   uint32_t nelems)
{
    intptr_t actual = 0;
    ringidx_t mask = lfr->mask;
    ringidx_t size = mask + 1;
    ringidx_t tail = __atomic_load_n(&lfr->tail, __ATOMIC_RELAXED);
    if (lfr->flags & P64_LFRING_F_SPENQ)
    {
	//Single-producer
	ringidx_t head = __atomic_load_n(&lfr->head, __ATOMIC_ACQUIRE);
	actual = MIN((intptr_t)(head + size - tail), (intptr_t)nelems);
	if (actual <= 0)
	{
	    return 0;
	}
	for (uint32_t i = 0; i < (uint32_t)actual; i++)
	{
	    assert(lfr->ring[tail & mask].idx == tail - size);
	    lfr->ring[tail & mask].ptr = *elems++;
	    lfr->ring[tail & mask].idx = tail;
	    tail++;
	}
	__atomic_store_n(&lfr->tail, tail, __ATOMIC_RELEASE);
	return (uint32_t)actual;
    }
    //Else lock-free multi-producer
restart:
    while ((uint32_t)actual < nelems &&
	   before(tail, __atomic_load_n(&lfr->head, __ATOMIC_ACQUIRE) + size))
    {
	union
	{
	    struct element e;
	    ptrpair_t pp;
	} old, neu;
	void *elem = elems[actual];
	struct element *slot = &lfr->ring[tail & mask];
	old.e.ptr = __atomic_load_n(&slot->ptr, __ATOMIC_RELAXED);
	old.e.idx = __atomic_load_n(&slot->idx, __ATOMIC_RELAXED);
	do
	{
	    if (UNLIKELY(old.e.idx != tail - size))
	    {
		if (old.e.idx != tail)
		{
		    //We are far behind, restart with fresh index
		    tail = cond_reload(tail, &lfr->tail);
		    goto restart;
		}
		else//old.e.idx == tail
		{
		    //Slot already enqueued
		    tail++;//Try next slot
		    goto restart;
		}
	    }
	    //Found slot that was used one lap back
	    //Try to enqueue next element
	    neu.e.ptr = elem;
	    neu.e.idx = tail;//Set idx on enqueue
	}
	while (!lockfree_compare_exchange_16((ptrpair_t *)slot,
						   &old.pp,//Updated on failure
						   neu.pp,
						   /*weak=*/true,
						   __ATOMIC_RELEASE,
						   __ATOMIC_RELAXED));
	//Enqueue succeeded
	actual++;
	tail++;//Continue with next slot
    }
    (void)cond_update(&lfr->tail, tail);
    return (uint32_t)actual;
}

static inline ringidx_t
find_tail(p64_lfring_t *lfr, ringidx_t head, ringidx_t tail)
{
    if (lfr->flags & P64_LFRING_F_SPENQ)
    {
	//Single-producer enqueue
	//Just return a freshly read tail pointer
	tail = __atomic_load_n(&lfr->tail, __ATOMIC_ACQUIRE);
	return tail;
    }
    //Multi-producer enqueue
    //Scan ring for new elements that have been written but not released
    ringidx_t mask = lfr->mask;
    ringidx_t size = mask + 1;
    while (before(tail, head + size) &&
	__atomic_load_n(&lfr->ring[tail & mask].idx, __ATOMIC_RELAXED) == tail)
    {
	tail++;
    }
    tail = cond_update(&lfr->tail, tail);
    return tail;
}

//Dequeue elements from head
uint32_t
p64_lfring_dequeue(p64_lfring_t *lfr,
		   void **restrict elems,
		   uint32_t nelems,
		   uint32_t *index)
{
    ringidx_t mask = lfr->mask;
    intptr_t actual;
    ringidx_t head = __atomic_load_n(&lfr->head, __ATOMIC_RELAXED);
    ringidx_t tail = __atomic_load_n(&lfr->tail, __ATOMIC_ACQUIRE);
    do
    {
	actual = MIN((intptr_t)(tail - head), (intptr_t)nelems);
	if (UNLIKELY(actual <= 0))
	{
	    //Ring looks empty, scan for new but unreleased elements
	    tail = find_tail(lfr, head, tail);
	    actual = MIN((intptr_t)(tail - head), (intptr_t)nelems);
	    if (actual <= 0)
	    {
		return 0;
	    }
	}
	for (uint32_t i = 0; i < (uint32_t)actual; i++)
	{
	    elems[i] = lfr->ring[(head + i) & mask].ptr;
	}
	smp_fence(LoadStore);//Order loads only
	if (UNLIKELY(lfr->flags & P64_LFRING_F_SCDEQ))
	{
	    //Single-consumer
	    __atomic_store_n(&lfr->head, head + actual, __ATOMIC_RELAXED);
	    break;
	}
	//Else lock-free multi-consumer
    }
    while (!__atomic_compare_exchange_n(&lfr->head,
					&head,//Updated on failure
					head + actual,
					/*weak*/false,
					__ATOMIC_RELAXED,
					__ATOMIC_RELAXED));
    *index = (uint32_t)head;
    return (uint32_t)actual;
}
