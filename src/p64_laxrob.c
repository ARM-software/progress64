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
#include "os_abstraction.h"

struct p64_laxrob
{
    p64_laxrob_elem_t *pending;
    p64_laxrob_cb cb;
    void *arg;
    uint32_t oldest;
    uint32_t size;
    uint32_t mask;
    uint32_t nvec;//Number of elems buffered in output vector
    uint32_t vecsz;//Size of output vector
    p64_laxrob_elem_t *ring[];//ROB ring buffer followed by output vector
};

//Address of output vector
#define ROB_VEC(rob) ((rob)->ring + (rob)->size)

#define IDLE 1
#define BUSY NULL
#define IS_IDLE(x) (((uintptr_t)(x) & IDLE) != 0)
#define IS_BUSY(x) (((uintptr_t)(x) & IDLE) == 0)
#define PEND_PTR(x) (p64_rob_elem_t *)((uintptr_t)(x) & ~IDLE)

p64_laxrob_t *
p64_laxrob_alloc(uint32_t nslots,
		 uint32_t vecsz,
		 p64_laxrob_cb cb,
		 void *arg)
{
    assert(IS_IDLE(IDLE));
    assert(!IS_IDLE(BUSY));
    assert(IS_BUSY(BUSY));
    assert(!IS_BUSY(IDLE));
    if (nslots < 1 || nslots > 0x80000000)
    {
	fprintf(stderr, "Invalid laxrob size %u\n", nslots);
	abort();
    }
    if (vecsz < 1 || vecsz > 0x80000000)
    {
	fprintf(stderr, "Invalid laxrob output vector size %u\n", vecsz);
	abort();
    }
    unsigned long ringsize = ROUNDUP_POW2(nslots);
    size_t nbytes = sizeof(p64_laxrob_t) +
		    (ringsize + vecsz) * sizeof(p64_laxrob_elem_t *);
    p64_laxrob_t *rob = p64_malloc(nbytes, CACHE_LINE);
    if (rob != NULL)
    {
	rob->pending = (p64_laxrob_elem_t *)IDLE;
	rob->oldest = 0;
	rob->size = ringsize;
	rob->mask = ringsize - 1;
	rob->cb = cb;
	rob->arg = arg;
	rob->nvec = 0;
	rob->vecsz = vecsz;
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
	p64_mfree(rob);
    }
}

static inline void
retire_list(p64_laxrob_t *rob, p64_laxrob_elem_t *list)
{
    do
    {
	assert(list->sn == rob->oldest);
	p64_laxrob_elem_t *elem = list;
	p64_laxrob_elem_t *next = list->next;
	list->next = NULL;
	//'elem' is single node
	assert(rob->nvec < rob->vecsz);
	ROB_VEC(rob)[rob->nvec++] = elem;
	if (rob->nvec == rob->vecsz)
	{
	    rob->cb(rob->arg, ROB_VEC(rob), rob->nvec);
	    rob->nvec = 0;
	}
	list = next;
    }
    while (list != NULL);
}

//Return number of elements retired
static inline void
retire_slots(p64_laxrob_t *rob, uint32_t nslots)
{
    //We only have to make one pass over the ring
    uint32_t nretire = nslots > rob->size ? rob->size : nslots;
    for (uint32_t i = 0; i < nretire; i++)
    {
	p64_laxrob_elem_t *list = rob->ring[rob->oldest & rob->mask];
	if (list != NULL)
	{
	    rob->ring[rob->oldest & rob->mask] = NULL;
	    retire_list(rob, list);
	}
	rob->oldest++;
    }
    //Update 'oldest' with the skipped slots
    rob->oldest += nslots - nretire;
}

#define BEFORE(sn, h) ((int32_t)(sn) - (int32_t)(h) < 0)
#define AFTER(sn, t) ((int32_t)(sn) - (int32_t)(t) >= 0)

static void
insert_elems(p64_laxrob_t *rob, p64_laxrob_elem_t *list)
{
    //Process 'list' list of elements
    while (list != NULL)
    {
	p64_laxrob_elem_t *elem = list;
	p64_laxrob_elem_t *next = list->next;
	list->next = NULL;
	//'elem' is single node
	if (UNLIKELY(BEFORE(elem->sn, rob->oldest)))
	{
	    //Element before oldest => straggler
	    retire_list(rob, elem);
	}
	else if (AFTER(elem->sn, rob->oldest + rob->size))
	{
	    //Element beyond newest element
	    //Move buffer to accomodate new element
	    uint32_t delta = elem->sn - (rob->oldest + rob->size - 1);
	    retire_slots(rob, delta);
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
	    elem->next = rob->ring[elem->sn & rob->mask];
	    rob->ring[elem->sn & rob->mask] = elem;
	}
	list = next;
    }
    if (rob->nvec != 0)
    {
	//Flush any buffered output
	rob->cb(rob->arg, ROB_VEC(rob), rob->nvec);
	rob->nvec = 0;
    }
}

static inline p64_laxrob_elem_t *
acquire_rob_or_enqueue(p64_laxrob_t *rob,
		       p64_laxrob_elem_t *list,
		       p64_laxrob_elem_t **last)
{
    int ret;
    p64_laxrob_elem_t *old, *neu;
    do
    {
	old = __atomic_load_n(&rob->pending, __ATOMIC_ACQUIRE);
	if (IS_BUSY(old))
	{
	    //ROB is busy, enqueue our elements
	    *last = old;//Join lists
	    neu = list;
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
	    //Undo any spurious join (whose CAS failed)
	    *last = NULL;
	}
    }
    while (UNLIKELY(!ret));
    assert(rob->nvec == 0);
    return IS_BUSY(old) ? NULL /*enqueued*/: list /*acquired*/;
}

static inline p64_laxrob_elem_t *
release_rob_or_dequeue(p64_laxrob_t *rob)
{
    assert(rob->nvec == 0);
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
		   IS_BUSY(old = (p64_laxrob_elem_t*)LDX((uintptr_t *)&rob->pending, __ATOMIC_ACQUIRE)))
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
    //Find last element in list
    p64_laxrob_elem_t **last = &list;
    while (*last != NULL)
    {
	last = &(*last)->next;
    }

    //Acquire ROB or enqueue elements
    list = acquire_rob_or_enqueue(rob, list, last);
    //'list' == NULL: we enqueued our elements for processing by current robber
    //'list' != NULL: we acquired ROB, now insert all elements
    while (list != NULL)
    {
	//Insert list of elements into ROB
	insert_elems(rob, list);

	//Release ROB or dequeue new elements
	list = release_rob_or_dequeue(rob);
	//list != NULL: new elements dequeued, insert them
    }
}

//Retire in-order elements and invoke the callback
void
p64_laxrob_flush(p64_laxrob_t *rob,
		 uint32_t nslots)
{
    //Acquire ROB (may block)
    acquire_rob(rob);

    retire_slots(rob, nslots);
    //Flush any buffered output
    if (rob->nvec != 0)
    {
	rob->cb(rob->arg, ROB_VEC(rob), rob->nvec);
	rob->nvec = 0;
    }

    for (;;)
    {
	//Release ROB or dequeue new elements
	p64_laxrob_elem_t *list = release_rob_or_dequeue(rob);
	//list == NULL: ROB released
	//list != NULL: new elements dequeued, now insert them
	if (list == NULL)
	{
	    return;
	}
	insert_elems(rob, list);
    }
}

#undef BEFORE
#undef AFTER
