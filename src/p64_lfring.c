//Copyright (c) 2017-2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "p64_lfring.h"
#include "build_config.h"

#include "arch.h"
#include "lockfree.h"
#ifndef __aarch64__
#define lockfree_compare_exchange_16_frail lockfree_compare_exchange_16
#endif
#include "common.h"
#ifdef USE_LDXSTX
#include "ldxstx.h"
#endif

typedef uint32_t ringidx_t;
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
    ringidx_t mask;
    struct element ring[] ALIGNED(CACHE_LINE);
} ALIGNED(CACHE_LINE);

p64_lfring_t *
p64_lfring_alloc(uint32_t nelems)
{
    unsigned long ringsz = ROUNDUP_POW2(nelems);
    if (nelems == 0 || ringsz == 0 || ringsz > 0x80000000)
    {
	fprintf(stderr, "Invalid number of elements %u\n", nelems), abort();
    }
    size_t nbytes = ROUNDUP(sizeof(p64_lfring_t) + ringsz * sizeof(void *),
			    CACHE_LINE);
    p64_lfring_t *lfr = aligned_alloc(CACHE_LINE, nbytes);
    if (lfr != NULL)
    {
	lfr->head = 0;
	lfr->tail = 0;
	lfr->mask = ringsz - 1;
	for (ringidx_t i = 0; i < ringsz; i++)
	{
	    lfr->ring[i].ptr = NULL;
	    lfr->ring[i].idx = i;
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
	    fprintf(stderr, "Lock-free ring %p is not empty\n", lfr);
	}
	free(lfr);
    }
}

//True if 'a' is before 'b' ('a' < 'b') in serial number arithmetic
static inline bool
before(ringidx_t a, ringidx_t b)
{
    return (int32_t)(a - b) < 0;
}

static inline void
cond_update(ringidx_t *loc, ringidx_t neu)
{
#ifndef USE_LDXSTX
    ringidx_t old = __atomic_load_n(loc, __ATOMIC_RELAXED);
#endif
    do
    {
#ifdef USE_LDXSTX
	ringidx_t old = ldx32(loc, __ATOMIC_RELAXED);
#endif
	if (before(neu, old))//neu < old
	{
	    break;
	}
	//Else neu > old, need to update *loc
    }
#ifdef USE_LDXSTX
    while (UNLIKELY(stx32(loc, neu, __ATOMIC_RELEASE)));
#else
    while (!__atomic_compare_exchange_n(loc,
					&old,//Updated on failure
					neu,
					/*weak=*/true,
					__ATOMIC_RELEASE,
					__ATOMIC_RELAXED));
#endif
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
    uint32_t actual = 0;
    ringidx_t mask = lfr->mask;
    ringidx_t size = mask + 1;
    ringidx_t idx = __atomic_load_n(&lfr->tail, __ATOMIC_RELAXED);
restart:
    while (actual < nelems &&
	   before(idx, __atomic_load_n(&lfr->head, __ATOMIC_ACQUIRE) + size))
    {
	union
	{
	    struct element e;
	    __int128 ui;
	} old, neu;
#ifndef USE_LDXSTX
	old.e.ptr = __atomic_load_n(&lfr->ring[idx & mask].ptr, __ATOMIC_RELAXED);
	old.e.idx = __atomic_load_n(&lfr->ring[idx & mask].idx, __ATOMIC_RELAXED);
#endif
	do
	{
#ifdef USE_LDXSTX
	    old.ui = ldx128((__int128 *)&lfr->ring[idx & mask],
			    __ATOMIC_RELAXED);
#endif
	    if (old.e.idx != idx)
	    {
		//Slot (enqueued and) dequeued again
		//We are far behind, restart with fresh index
		idx = cond_reload(idx, &lfr->tail);
		goto restart;
	    }
	    if (old.e.ptr != NULL)
	    {
		//Slot already enqueued
		idx++;//Try next slot
		goto restart;
	    }
	    //Found empty slot
	    //Try to enqueue next element
	    neu.e.ptr = elems[actual];
	    neu.e.idx = old.e.idx;//Keep idx on enqueue
	}
#ifdef USE_LDXSTX
	while (unlikely(stx128((__int128 *)&lfr->ring[idx & mask],
			       neu.ui,
			       __ATOMIC_RELEASE)));
#else
	while (!lockfree_compare_exchange_16_frail((__int128 *)&lfr->ring[idx & mask],
						   &old.ui,//Updated on failure
						   neu.ui,
						   /*weak=*/true,
						   __ATOMIC_RELEASE,
						   __ATOMIC_RELAXED));
#endif
	//Enqueue succeeded
	actual++;
	idx++;//Continue with next slot
    }
    cond_update(&lfr->tail, idx);
    return actual;
}

//Dequeue elements from head
uint32_t
p64_lfring_dequeue(p64_lfring_t *lfr,
		   void **restrict elems,
		   uint32_t nelems)
{
    uint32_t actual = 0;
    ringidx_t mask = lfr->mask;
    ringidx_t size = mask + 1;
    ringidx_t idx = __atomic_load_n(&lfr->head, __ATOMIC_RELAXED);
restart:
    while (actual < nelems &&
	   before(idx, __atomic_load_n(&lfr->tail, __ATOMIC_ACQUIRE)))
    {
	union
	{
	    struct element e;
	    __int128 ui;
	} old, neu;
#ifndef USE_LDXSTX
	old.e.ptr = __atomic_load_n(&lfr->ring[idx & mask].ptr, __ATOMIC_RELAXED);
	old.e.idx = __atomic_load_n(&lfr->ring[idx & mask].idx, __ATOMIC_RELAXED);
#endif
	do
	{
#ifdef USE_LDXSTX
	    old.ui = ldx128((__int128 *)&lfr->ring[idx & mask],
			    __ATOMIC_ACQUIRE);
#endif
	    if (old.e.ptr == NULL)
	    {
		//Slot already dequeued
		if (old.e.idx != idx + size)
		{
		    //We are far behind, restart
		    idx = cond_reload(idx, &lfr->head);
		    goto restart;
		}
		else
		{
		    idx++;//Try next slot
		    goto restart;
		}
	    }
	    else //Found used slot
	    {
		if (old.e.idx != idx)
		{
		    //We are far behind, restart
		    idx = cond_reload(idx, &lfr->head);
		    goto restart;
		}
	    }
	    //Try to dequeue element
	    neu.e.ptr = NULL;
	    neu.e.idx = idx + size;//Increment idx on dequeue
	}
#ifdef USE_LDXSTX
	while (unlikely(stx128((__int128 *)&lfr->ring[idx & mask],
			       neu.ui,
			       __ATOMIC_RELAXED)));
#else
	while (!lockfree_compare_exchange_16_frail((__int128 *)&lfr->ring[idx & mask],
						   &old.ui,//Updated on failure
						   neu.ui,
						   /*weak=*/true,
						   __ATOMIC_ACQUIRE,
						   __ATOMIC_RELAXED));
#endif
	//Dequeue succeeded
	elems[actual++] = old.e.ptr;
	idx++;//Continue with next slot
    }
    cond_update(&lfr->head, idx);
    return actual;
}
