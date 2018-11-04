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
#include "common.h"
#ifdef USE_LDXSTX
#include "ldxstx.h"
#endif

typedef uint32_t ringidx_t;

struct p64_lfring
{
    ringidx_t head;
#ifdef USE_SPLIT_PRODCONS
    ringidx_t tail ALIGNED(CACHE_LINE);
#else
    ringidx_t tail;
#endif
    ringidx_t mask;
    void *ring[] ALIGNED(CACHE_LINE);
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
	for (uint32_t i = 0; i < ringsz; i++)
	{
	    lfr->ring[i] = NULL;
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
    while (unlikely(stx32(loc, neu, __ATOMIC_RELEASE)));
#else
    while (!__atomic_compare_exchange_n(loc,
                                        &old,//Updated on failure
                                        neu,
                                        /*weak=*/false,
                                        __ATOMIC_RELEASE,
                                        __ATOMIC_RELAXED));
#endif
}

//Enqueue elements at tail
uint32_t
p64_lfring_enqueue(p64_lfring_t *lfr,
		   void *const *restrict elems,
		   uint32_t nelems)
{
    ringidx_t mask = lfr->mask;
    ringidx_t size = mask + 1;
    ringidx_t idx = __atomic_load_n(&lfr->tail, __ATOMIC_RELAXED);
    int actual = 0;
    while (actual < nelems &&
	   before(idx, __atomic_load_n(&lfr->head, __ATOMIC_ACQUIRE) + size))
    {
	__builtin_prefetch(&lfr->ring[idx & mask], 1, 0);
	void *elem = __atomic_load_n(&lfr->ring[idx & mask], __ATOMIC_RELAXED);
	if (elem == NULL)
	{
	    //Found empty slot
	    //Try to enqueue next element
	    if (__atomic_compare_exchange_n(&lfr->ring[idx & mask],
					    &elem,
					    elems[actual],
					    /*weak=*/false,
					    __ATOMIC_RELEASE,
					    __ATOMIC_RELAXED))
	    {
		//CAS succeeded
		actual++;
	    }
	    else
	    {
		//Else CAS failed, someone else took the slot
		//Active contention, re-read tail for a better index
		ringidx_t tail = __atomic_load_n(&lfr->tail, __ATOMIC_RELAXED);
		if (before(idx, tail))
		{
		    //tail > idx, update idx
		    idx = tail;
		    continue;
		}
		//Else idx >= tail
	    }
	}
	///Slot is now used, either by us or by someone else
	//Continue with next slot
	idx++;
    }
    if (actual != 0)
    {
	cond_update(&lfr->tail, idx);
    }
    return actual;
}

//Dequeue elements from head
uint32_t
p64_lfring_dequeue(p64_lfring_t *lfr,
		   void **restrict elems,
		   uint32_t nelems,
		   uint32_t *restrict idxs)
{
    ringidx_t mask = lfr->mask;
    ringidx_t idx = __atomic_load_n(&lfr->head, __ATOMIC_RELAXED);
    int actual = 0;
    while (actual < nelems &&
           before(idx, __atomic_load_n(&lfr->tail, __ATOMIC_ACQUIRE)))
    {
        void *elem = __atomic_load_n(&lfr->ring[idx & mask], __ATOMIC_RELAXED);
        while (elem != NULL)
        {
            //Found used slot
            //Try to take element
            if (__atomic_compare_exchange_n(&lfr->ring[idx & mask],
                                            &elem,//Updated on failure
                                            NULL,
                                            /*weak=*/false,
                                            __ATOMIC_ACQUIRE,
                                            __ATOMIC_RELAXED))
            {
                //CAS succeeded
                elems[actual] = elem;
                idxs[actual] = idx;
		actual++;
                break;
            }
            //Else CAS failed, 'elem' updated
            //Possibly 'elem' has non-NULL value so try again
        }
        ///Slot is now free, either by us or by someone else
        //Continue with next slot
        idx++;
    }
    if (actual != 0)
    {
        cond_update(&lfr->head, idx);
    }
    return actual;
}
