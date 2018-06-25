//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "p64_laxrob.h"
#include "build_config.h"

#include "arch.h"
#include "common.h"

struct p64_laxrob
{
    p64_laxrob_elem_t *pending;
    uint32_t oldest;
    uint32_t size;
    uint32_t mask;
    p64_laxrob_cb cb;
    void *arg;
    void *ring[] ALIGNED(CACHE_LINE);
};

#define IDLE 1
#define BUSY NULL
#define IS_IDLE(x) (((uintptr_t)(x) & IDLE) != 0)
#define IS_BUSY(x) (((uintptr_t)(x) & IDLE) == 0)
#define PEND_PTR(x) (p64_rob_elem_t *)((uintptr_t)(x) & ~IDLE)

p64_laxrob_t *
p64_laxrob_alloc(uint32_t nelems,
	      p64_laxrob_cb cb,
	      void *arg)
{
    assert(IS_IDLE(IDLE));
    assert(!IS_IDLE(BUSY));
    assert(IS_BUSY(BUSY));
    assert(!IS_BUSY(IDLE));
    if (nelems < 1 || nelems > 0x80000000)
    {
	fprintf(stderr, "Invalid reorder buffer size %u\n", nelems), abort();
    }
    unsigned long ringsize = ROUNDUP_POW2(nelems);
    size_t nbytes = ROUNDUP(sizeof(p64_laxrob_t) + ringsize * sizeof(void *),
			    CACHE_LINE);
    p64_laxrob_t *rob = aligned_alloc(CACHE_LINE, nbytes);
    if (rob != NULL)
    {
	rob->pending = (p64_laxrob_elem_t *)IDLE;
	rob->oldest = 0;
	rob->size = ringsize;
	rob->mask = ringsize - 1;
	rob->cb = cb;
	rob->arg = arg;
	for (uint32_t i = 0; i < ringsize; i++)
	{
	    rob->ring[i] = NULL;
	}
	assert(IS_IDLE(rob->pending));
	assert(!IS_BUSY(rob->pending));
	return rob;
    }
    return NULL;
}

void
p64_laxrob_free(p64_laxrob_t *rob)
{
    if (rob != NULL)
    {
	assert(IS_IDLE(rob->pending));
	for (uint32_t i = 0; i < rob->size; i++)
	{
	    if (rob->ring[(rob->oldest + i) & rob->mask] != NULL)
	    {
		fprintf(stderr, "Reorder buffer %p is not empty\n", rob);
		abort();
	    }
	}
	free(rob);
    }
}

//Return number of (non-empty) slots retired
static uint32_t
retire_elems(p64_laxrob_t *rob, uint32_t nelems)
{
    uint32_t retired = 0;
    //We only have to make one pass over the ring
    uint32_t nretire = nelems > rob->size ? rob->size : nelems;
    for (uint32_t i = 0; i < nretire; i++)
    {
	p64_laxrob_elem_t *elem = rob->ring[(rob->oldest + i) & rob->mask];
	if (elem != NULL)
	{
	    assert(elem->sn == rob->oldest + i);
	    rob->cb(rob->arg, elem);
	    retired++;
	    rob->ring[(rob->oldest + i) & rob->mask] = NULL;
	}
    }
    rob->oldest += nelems;
    return retired;
}

#define BEFORE(sn, h) ((int32_t)(sn) - (int32_t)(h) < 0)
#define AFTER(sn, t) ((int32_t)(sn) - (int32_t)(t) >= 0)

static void
insert_elems(p64_laxrob_t *rob, p64_laxrob_elem_t *elem)
{
    //Process 'elem' list of elements
    uint32_t retired = 0;
    while (elem != NULL)
    {
	p64_laxrob_elem_t *next = elem->next;
	elem->next = NULL;
	if (UNLIKELY(BEFORE(elem->sn, rob->oldest)))
	{
	    //Element before oldest => straggler
	    rob->cb(rob->arg, elem);
	    retired++;
	}
	else if (AFTER(elem->sn, rob->oldest + rob->size))
	{
	    //Element beyond newest element
	    //Move buffer to accomodate new element
	    uint32_t delta = elem->sn - (rob->oldest + rob->size - 1);
	    retired += retire_elems(rob, delta);
	    assert(!BEFORE(elem->sn, rob->oldest));
	    assert(!AFTER(elem->sn, rob->oldest + rob->size));
	    //Insert element into ring
	    assert(rob->ring[elem->sn & rob->mask] == NULL);
	    rob->ring[elem->sn & rob->mask] = elem;
	}
	else
	{
	    //Element inside buffer
	    //Insert element at head of list at ROB slot
	    __builtin_prefetch(&rob->ring[elem->sn & rob->mask], 1, 0);
	    elem->next = rob->ring[elem->sn & rob->mask];
	    rob->ring[elem->sn & rob->mask] = elem;
	}
	elem = next;
    }
    if (retired != 0)
    {
	//Notify all done for now (flush any buffered output)
	rob->cb(rob->arg, NULL);
    }
}

static inline p64_laxrob_elem_t *
acquire_rob_or_enqueue(p64_laxrob_t *rob, p64_laxrob_elem_t *elem)
{
    //Find last element in list
    p64_laxrob_elem_t *last = elem;
    while (last->next != NULL)
    {
	last = last->next;
    }

    int ret;
    p64_laxrob_elem_t *old, *neu;
    do
    {
	old = __atomic_load_n(&rob->pending, __ATOMIC_ACQUIRE);
	if (IS_BUSY(old))
	{
	    //ROB is busy, enqueue our elements
	    last->next = old;
	    neu = elem;
	    assert(IS_BUSY(neu));
	    ret = __atomic_compare_exchange_n(&rob->pending,
					      &old,
					      neu,
					      /*weak=*/true,
					      __ATOMIC_RELEASE,
					      __ATOMIC_RELAXED);
					    
	}
	else
	{
	    //ROB is idle, try to acquire ROB
	    neu = BUSY;
	    assert(IS_BUSY(neu));
	    ret = __atomic_compare_exchange_n(&rob->pending,
					      &old,
					      neu,
					      /*weak=*/true,
					      __ATOMIC_RELAXED,
					      __ATOMIC_RELAXED);
	    //Possibly we executed 'last->next = old' earlier in a failed CAS
	    last->next = NULL;
	}
    }
    while (UNLIKELY(!ret));
    return IS_BUSY(old) ? NULL /*enqueued*/: elem /*acquired*/;
}

static inline p64_laxrob_elem_t *
release_rob_or_dequeue(p64_laxrob_t *rob)
{
    int ret;
    p64_laxrob_elem_t *old, *neu;
    do
    {
	old = __atomic_load_n(&rob->pending, __ATOMIC_ACQUIRE);
	assert(IS_BUSY(old));
	if (old == BUSY)//old == NULL
	{
	    //No new elements enqueued, release ROB
	    neu = (p64_laxrob_elem_t *)IDLE;
	    assert(IS_IDLE(neu));
	    ret = __atomic_compare_exchange_n(&rob->pending,
					      &old,
					      neu,
					      /*weak=*/true,
					      __ATOMIC_RELEASE,
					      __ATOMIC_RELAXED);
	}
	else
	{
	    //New elements enqueued, grab them while keeping ROB
	    neu = BUSY;
	    assert(IS_BUSY(neu));
	    ret = __atomic_compare_exchange_n(&rob->pending,
					      &old,
					      neu,
					      /*weak=*/true,
					      __ATOMIC_RELAXED,
					      __ATOMIC_RELAXED);
	}
    }
    while (UNLIKELY(!ret));
    return old;
}

//Retire in-order elements and invoke the callback
static inline void
acquire_rob(p64_laxrob_t *rob)
{
    p64_laxrob_elem_t *old, *neu;
    do
    {
	old = __atomic_load_n(&rob->pending, __ATOMIC_ACQUIRE);
	if (IS_BUSY(old))
	{
	    //ROB is busy, wait until idle
	    SEVL();
	    while (WFE() &&
		   IS_BUSY(old = (p64_laxrob_elem_t*)LDXR64((uintptr_t *)&rob->pending, __ATOMIC_ACQUIRE)))
	    {
		DOZE();
	    }
	}
	//ROB is idle, try to acquire ROB
	neu = BUSY;
	assert(IS_IDLE(old));
	assert(IS_BUSY(neu));
    }
    while (UNLIKELY(!__atomic_compare_exchange_n(&rob->pending,
						 &old,
						 neu,
						 /*weak=*/true,
						 __ATOMIC_RELAXED,
						 __ATOMIC_RELAXED)));
}

void
p64_laxrob_insert(p64_laxrob_t *rob,
		  p64_laxrob_elem_t *list)
{
    //Acquire ROB or enqueue elements
    list = acquire_rob_or_enqueue(rob, list);
    //Elem == NULL => we enqueued our elements for processing by current robber
    //Elem != NULL => we acquired ROB, insert our elements
    while (list != NULL)
    {
	//Insert list of elements into ROB
	insert_elems(rob, list);

	//Release ROB or dequeue new elements
	list = release_rob_or_dequeue(rob);
	//list != NULL => new elements dequeued, insert them
    }
}

void
p64_laxrob_flush(p64_laxrob_t *rob,
		 uint32_t nelems)
{
    //Acquire ROB (may block)
    acquire_rob(rob);

    if (retire_elems(rob, nelems) != 0)
    {
	//Notify all done for now (flush any buffered output)
	rob->cb(rob->arg, NULL);
    }

    //Release ROB
    for (;;)
    {
	//Release ROB or dequeue new elements
	p64_laxrob_elem_t *list = release_rob_or_dequeue(rob);
	//list != NULL => new elements dequeued, insert them
	if (list == NULL)
	{
	    return;
	}
	insert_elems(rob, list);
    }
}
