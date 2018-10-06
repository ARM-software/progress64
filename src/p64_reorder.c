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

struct p64_reorder
{
    //Written by p64_reorder_release()
    struct hi hi ALIGNED(CACHE_LINE);//head and chgi
    //Constants
    uint32_t mask;
    bool user_acquire;
    p64_reorder_cb cb;
    void *arg;
    //Written by p64_reorder_acquire()
    uint32_t tail ALIGNED(CACHE_LINE);
    //Written by p64_reorder_release()
    void *ring[] ALIGNED(CACHE_LINE);
};

p64_reorder_t *
p64_reorder_alloc(uint32_t nelems,
		  bool user_acquire,
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
    p64_reorder_t *rob = aligned_alloc(CACHE_LINE, nbytes);
    if (rob != NULL)
    {
	//Clear the ring pointers
	memset(rob, 0, offsetof(p64_reorder_t, ring));
	rob->hi.head = 0;
	rob->hi.chgi = 0;
	rob->mask = ringsize - 1;
	rob->user_acquire = user_acquire;
	rob->cb = cb;
	rob->arg = arg;
	rob->tail = 0;
	for (unsigned long i = 0; i < ringsize; i++)
	{
	    rob->ring[i] = NULL;
	}
	return rob;
    }
    return NULL;
}

void
p64_reorder_free(p64_reorder_t *rob)
{
    if (rob != NULL)
    {
	if (!rob->user_acquire && rob->hi.head != rob->tail)
	{
	    fprintf(stderr, "Reorder buffer %p is not empty\n", rob), abort();
	}
	free(rob);
    }
}

uint32_t
p64_reorder_acquire(p64_reorder_t *rob,
		    uint32_t requested,
		    uint32_t *sn)
{
    uint32_t head, tail;
    int32_t actual;
    PREFETCH_FOR_WRITE(&rob->tail);
    tail = __atomic_load_n(&rob->tail, __ATOMIC_RELAXED);
    do
    {
	head = __atomic_load_n(&rob->hi.head, __ATOMIC_ACQUIRE);
	int32_t available = (rob->mask + 1) - (tail - head);
	//Use signed arithmetic for robustness as head & tail are not read
	//atomically, available may be < 0
	actual = MIN(available, (int32_t)requested);
	if (UNLIKELY(actual <= 0))
	{
	    return 0;
	}
    }
    while (!__atomic_compare_exchange_n(&rob->tail,
					&tail,//Updated on failure
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
p64_reorder_release(p64_reorder_t *rob,
		    uint32_t sn,
		    void *elems[],
		    uint32_t nelems)
{
    uint32_t mask = rob->mask;
    p64_reorder_cb cb = rob->cb;
    void *arg = rob->arg;
    if (rob->user_acquire)
    {
	//With user_acquire, the user might have been generous and allocated
	//a SN currently outside of the ROB window
	uint32_t sz = mask + 1;
	if (UNLIKELY(AFTER(sn + nelems,
			   __atomic_load_n(&rob->hi.head, __ATOMIC_ACQUIRE) + sz)))
	{
	    //We must wait for in-order elements to be retired so that our SN
	    //will fit inside the ROB window
	    SEVL();
	    while (WFE() && AFTER(sn + nelems,
				  LDXR32(&rob->hi.head, __ATOMIC_ACQUIRE) + sz))
	    {
		DOZE();
	    }
	}
    }
    else if (UNLIKELY(AFTER(sn + nelems, rob->tail)))
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
	assert(rob->ring[(sn + i) & mask] == NULL);
	__atomic_store_n(&rob->ring[(sn + i) & mask], elems[i],
			 __ATOMIC_RELAXED);
    }

    struct hi old;
    __atomic_load(&rob->hi, &old, __ATOMIC_ACQUIRE);
    while (BEFORE(old.head, sn) || !BEFORE(old.head, sn + nelems))
    {
	//We are out-of-order
	//Update chgi to indicate presence of new elements
	struct hi new;
	new.head = old.head;
	new.chgi = old.chgi + 1;//Unique value
	//Update head&chgi, fail if any has changed
	if (__atomic_compare_exchange(&rob->hi,
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
	while ((elem = __atomic_load_n(&rob->ring[new.head & mask],
				       __ATOMIC_ACQUIRE)) != NULL)
	{
	    rob->ring[new.head & mask] = NULL;
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
    while (!__atomic_compare_exchange(&rob->hi,
				      &old,//Updated on failure
				      &new,
				      /*weak=*/true,
				      __ATOMIC_RELEASE,//Release ring updates
				      __ATOMIC_ACQUIRE));
}
