//Copyright (c) 2016, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "p64_reorder.h"
#include "build_config.h"

#include "arch.h"
#include "common.h"

struct hi
{
    uint32_t head;//First missing element
    uint32_t chgi;//Change indicator
} ALIGNED(sizeof(uint64_t));

//p64_reorder_reserve() writes tail
//p64_reorder_insert() writes head & chgi
struct p64_reorder
{
    struct hi hi ALIGNED(CACHE_LINE);//head and chgi
    uint32_t mask;
    p64_reorder_cb cb;
    void *arg;
    uint32_t tail ALIGNED(CACHE_LINE);
    void *ring[] ALIGNED(CACHE_LINE);
};

p64_reorder_t *
p64_reorder_alloc(uint32_t nelems,
		  p64_reorder_cb cb,
		  void *arg)
{
    if (nelems < 1 || nelems > 0x80000000)
    {
	fprintf(stderr, "Invalid reorder buffer size %u\n", nelems), abort();
    }
    unsigned long ringsize = ROUNDUP_POW2(nelems);
    size_t nbytes = ROUNDUP(sizeof(p64_reorder_t) + ringsize * sizeof(void *),
			    CACHE_LINE);
    p64_reorder_t *rb = aligned_alloc(CACHE_LINE, nbytes);
    if (rb != NULL)
    {
	//Clear the ring pointers
	memset(rb, 0, nbytes);
	rb->hi.head = 0;
	rb->hi.chgi = 0;
	rb->mask = ringsize - 1;
	rb->cb = cb;
	rb->arg = arg;
	rb->tail = 0;
	return rb;
    }
    return NULL;
}

void
p64_reorder_free(p64_reorder_t *rb)
{
    if (rb != NULL)
    {
	if (rb->hi.head != rb->tail)
	{
	    fprintf(stderr, "Reorder buffer %p is not empty\n", rb), abort();
	}
	free(rb);
    }
}

uint32_t
p64_reorder_reserve(p64_reorder_t *rb,
		    uint32_t requested,
		    uint32_t *sn)
{
    uint32_t head, tail;
    int32_t actual;
    do
    {
	head = __atomic_load_n(&rb->hi.head, __ATOMIC_RELAXED);
	tail = __atomic_load_n(&rb->tail, __ATOMIC_RELAXED);
	int32_t available = (rb->mask + 1) - (tail - head);
	//Use signed arithmetic for robustness as head & tail are not read
	//atomically, available may be < 0
	actual = MIN(available, (int32_t)requested);
	if (UNLIKELY(actual <= 0))
	{
	    return 0;
	}
    }
    while (!__atomic_compare_exchange_n(&rb->tail,
					&tail,
					tail + actual,
					/*weak=*/true,
					__ATOMIC_RELAXED,
					__ATOMIC_RELAXED));
    *sn = tail;
    return actual;
}

#define BEFORE(sn, h) ((int32_t)(sn) - (int32_t)(h) < 0)
#define AFTER(sn, t) ((int32_t)(sn) - (int32_t)(t) > 0)

void
p64_reorder_insert(p64_reorder_t *rb,
		   uint32_t nelems,
		   void *elems[],
		   uint32_t sn)
{
    __builtin_prefetch(&rb->hi, 1, 0);
    uint32_t mask = rb->mask;
    p64_reorder_cb cb = rb->cb;
    void *arg = rb->arg;
    if (UNLIKELY(AFTER(sn + nelems, rb->tail)))
    {
	fprintf(stderr, "Invalid sequence number)\n"), abort();
    }
    //Store our elements in reorder buffer
    SMP_WMB();//Explicit memory barrier so we can use store-relaxed below
    for (uint32_t i = 0; i < nelems; i++)
    {
	if (UNLIKELY(elems[i] == NULL))
	{
	    fprintf(stderr, "Invalid NULL element\n"), abort();
	}
	assert(rb->ring[(sn + i) & mask] == NULL);
	__atomic_store_n(&rb->ring[(sn + i) & mask], elems[i],
			 __ATOMIC_RELAXED);
    }

    struct hi old;
    __atomic_load(&rb->hi, &old, __ATOMIC_ACQUIRE);
    while (BEFORE(sn, old.head) || AFTER(sn + nelems - 1, old.head))
    {
	//We are out-of-order
	//Update chgi to indicate presence of new elements
	struct hi new;
	new.head = old.head;
	new.chgi = old.chgi + 1;//Unique value
	//Update head&chgi, fail if any has changed
	if (__atomic_compare_exchange(&rb->hi,
				      &old,//Updated on failure
				      &new,
				      /*weak=*/false,
				      __ATOMIC_RELEASE,
				      __ATOMIC_RELAXED))
	{
	    //CAS succeeded => head same (we are not in-order), chgi updated
	    return;
	}
	//CAS failed => head and/or chgi changed
	//We might not be out-of-order anymore
    }

    assert(!BEFORE(sn, old.head) && !AFTER(sn + nelems - 1, old.head));
    //We are in-order so our responsibility to retire elements
    struct hi new;
    new.head = old.head;
    //Scan ring to find consecutive in-order elements and retire them
    do
    {
	void *elem;
	while ((elem = __atomic_load_n(&rb->ring[new.head & mask],
				       __ATOMIC_ACQUIRE)) != NULL)
	{
	    rb->ring[new.head & mask] = NULL;
	    cb(arg, elem);
	    new.head++;
	}
	assert(new.head != old.head);
	new.chgi = old.chgi;
	__builtin_prefetch(&rb->hi, 1, 0);
    }
    //Update head&chgi, fail if chgi has changed (head cannot change)
    while (!__atomic_compare_exchange(&rb->hi,
				      &old,//Updated on failure
				      &new,
				      /*weak=*/false,
				      __ATOMIC_RELEASE,//Release ring updates
				      __ATOMIC_RELAXED));
}
