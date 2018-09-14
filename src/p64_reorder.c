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
    bool user_reserve;
    p64_reorder_cb cb;
    void *arg;
    uint32_t tail ALIGNED(CACHE_LINE);
    void *ring[] ALIGNED(CACHE_LINE);
};

p64_reorder_t *
p64_reorder_alloc(uint32_t nelems,
		  bool user_reserve,
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
	rb->user_reserve = user_reserve;
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
	if (!rb->user_reserve && rb->hi.head != rb->tail)
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
    PREFETCH_FOR_WRITE(&rb->tail);
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

static inline bool
BEFORE(uint32_t x, uint32_t y)
{
    return (int32_t)((x) - (y)) < 0;
}

static inline bool
AFTER(uint32_t x, uint32_t y)
{
    return (int32_t)((x) - (y)) > 0;
}

void
p64_reorder_insert(p64_reorder_t *rb,
		   uint32_t nelems,
		   void *elems[],
		   uint32_t sn)
{
    uint32_t mask = rb->mask;
    p64_reorder_cb cb = rb->cb;
    void *arg = rb->arg;
    if (rb->user_reserve)
    {
	//With user reserve, the user might have been generous and allocated
	//a SN currently outside of the ROB window
	uint32_t sz = mask + 1;
	if (UNLIKELY(AFTER(sn + nelems,
			   __atomic_load_n(&rb->hi.head, __ATOMIC_ACQUIRE) + sz)))
	{
	    //We must wait for in-order elements to be retired so that our SN will
	    //fit inside the ROB window
	    SEVL();
	    while (WFE() && AFTER(sn + nelems,
				  LDXR32(&rb->hi.head, __ATOMIC_ACQUIRE) + sz))
	    {
		DOZE();
	    }
	}
    }
    else if (UNLIKELY(AFTER(sn + nelems, rb->tail)))
    {
	fprintf(stderr, "Invalid sequence number %u\n", sn + nelems), abort();
    }
    //Store our elements in reorder buffer, releasing them
    //Separate release fence so we can use store-relaxed below
    __atomic_thread_fence(__ATOMIC_RELEASE);
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
    while (BEFORE(old.head, sn) || !BEFORE(old.head, sn + nelems))
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
				      /*weak=*/true,
				      __ATOMIC_RELEASE,
				      __ATOMIC_ACQUIRE))
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
    uint32_t npending = 0;
    //Scan ring to find consecutive in-order elements and retire them
    do
    {
	void *elem;
	while ((elem = __atomic_load_n(&rb->ring[new.head & mask],
				       __ATOMIC_ACQUIRE)) != NULL)
	{
	    rb->ring[new.head & mask] = NULL;
	    if (LIKELY((uintptr_t)elem > (uintptr_t)P64_REORDER_DUMMY))
	    {
		cb(arg, elem, new.head);
		npending++;
	    }
	    new.head++;
	}
	assert(new.head != old.head);
	if (LIKELY(npending != 0))
	{
	    cb(arg, NULL, new.head);
	    npending = 0;
	}
	new.chgi = old.chgi;
    }
    //Update head&chgi, fail if chgi has changed (head cannot change)
    while (!__atomic_compare_exchange(&rb->hi,
				      &old,//Updated on failure
				      &new,
				      /*weak=*/true,
				      __ATOMIC_RELEASE,//Release ring updates
				      __ATOMIC_ACQUIRE));
}
