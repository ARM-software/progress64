//Copyright (c) 2017-2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "p64_ringbuf.h"
#include "build_config.h"

#include "arch.h"
#include "common.h"
#ifdef USE_LDXSTX
#include "ldxstx.h"
#endif

#if defined USE_SPLIT_HEADTAIL && !defined USE_SPLIT_PRODCONS
#error USE_SPLIT_HEADTAIL not supported without USE_SPLIT_PRODCONS
#endif

#define SUPPORTED_FLAGS (P64_RINGBUF_F_SPENQ | P64_RINGBUF_F_MPENQ | \
			 P64_RINGBUF_F_SCDEQ | P64_RINGBUF_F_MCDEQ | \
			 P64_RINGBUF_F_LFDEQ)

#define FLAG_MTSAFE   0x0001
#define FLAG_LOCKFREE 0x0002

typedef uint32_t ringidx_t;

struct headtail
{
#if defined USE_SPLIT_PRODCONS
    ringidx_t head ALIGNED(CACHE_LINE);//tail for consumer
#else
    ringidx_t head;//tail for consumer
#endif
#if defined USE_SPLIT_HEADTAIL
    ringidx_t tail ALIGNED(CACHE_LINE);//head for consumer
#else
    ringidx_t tail;//head for consumer
#endif
    ringidx_t mask;
    uint32_t flags;
};

struct p64_ringbuf
{
    struct headtail prod;
    struct headtail cons;//NB head & tail are swapped for consumer metadata
    void *ring[] ALIGNED(CACHE_LINE);
} ALIGNED(CACHE_LINE);

p64_ringbuf_t *
p64_ringbuf_alloc(uint32_t nelems, uint32_t flags, size_t esize)
{
    unsigned long ringsz = ROUNDUP_POW2(nelems);
    if (nelems == 0 || ringsz == 0 || ringsz > 0x80000000)
    {
	fprintf(stderr, "Invalid number of elements %u\n", nelems), abort();
    }
    if ((flags & ~SUPPORTED_FLAGS) != 0)
    {
	fprintf(stderr, "Invalid flags %x\n", flags), abort();
    }
    size_t nbytes = ROUNDUP(sizeof(p64_ringbuf_t) + ringsz * esize, CACHE_LINE);
    p64_ringbuf_t *rb = aligned_alloc(CACHE_LINE, nbytes);
    if (rb != NULL)
    {
	rb->prod.head = 0;
	rb->prod.tail = 0;
	rb->prod.mask = ringsz - 1;
	rb->prod.flags = (flags & P64_RINGBUF_F_SPENQ) ? 0 : FLAG_MTSAFE;
	rb->cons.head = 0;
	rb->cons.tail = 0;
	rb->cons.mask = ringsz - 1;
	rb->cons.flags = (flags & P64_RINGBUF_F_SCDEQ) ? 0 : FLAG_MTSAFE;
	rb->cons.flags |= (flags & P64_RINGBUF_F_LFDEQ) ? FLAG_LOCKFREE : 0;
	return rb;
    }
    return NULL;
}

void *
p64_ringbuf_alloc_(uint32_t nelems, uint32_t flags, size_t esize)
{
    p64_ringbuf_t *rb = p64_ringbuf_alloc(nelems, flags, esize);
    if (rb != NULL)
    {
	return &rb->ring;
    }
    return NULL;
}

void
p64_ringbuf_free(p64_ringbuf_t *rb)
{
    if (rb != NULL)
    {
	if (rb->prod.head != rb->cons.head/*cons.tail*/)
	{
	    fprintf(stderr, "Ring buffer %p is not empty\n", rb);
	}
	free(rb);
    }
}

#define container_of(pointer, type, member) \
    ((type *)(void *)(((char *)pointer) - offsetof(type, member)))

void
p64_ringbuf_free_(void *ptr)
{
    if (ptr != NULL)
    {
	p64_ringbuf_free(container_of(ptr, p64_ringbuf_t, ring));
    }
}

//MT-unsafe single producer/consumer code
static inline p64_ringbuf_result_t
acquire_slots(const ringidx_t *headp,
	      ringidx_t *tailp,
	      ringidx_t mask,
	      int n,
	      bool enqueue)
{
    ringidx_t ring_size = enqueue ? /*enqueue*/mask + 1 : /*dequeue*/0;
    ringidx_t tail = __atomic_load_n(tailp, __ATOMIC_RELAXED);
    ringidx_t head = __atomic_load_n(headp, __ATOMIC_ACQUIRE);
    int actual = MIN(n, (int)(ring_size + head - tail));
    if (UNLIKELY(actual <= 0))
    {
	return (p64_ringbuf_result_t){ .index = 0, .actual = 0, .mask = mask };
    }
    return (p64_ringbuf_result_t){ .index = tail, .actual = actual, .mask = mask };
}

//MT-safe multi producer/consumer code
static inline p64_ringbuf_result_t
acquire_slots_mtsafe(struct headtail *rb,
		     int n,
		     bool enqueue)
{
    ringidx_t tail;
    ringidx_t ring_size = enqueue ? /*enqueue*/rb->mask + 1 : /*dequeue*/0;
    int actual;
#ifndef USE_LDXSTX
    tail = __atomic_load_n(&rb->tail, __ATOMIC_RELAXED);
#endif
    ringidx_t head = __atomic_load_n(&rb->head, __ATOMIC_ACQUIRE);
    do
    {
#ifdef USE_LDXSTX
	tail = ldx32(&rb->tail, __ATOMIC_RELAXED);
#endif
	actual = MIN(n, (int)(ring_size + head - tail));
	if (UNLIKELY(actual <= 0))
	{
	    return (p64_ringbuf_result_t){ .index = 0, .actual = 0, .mask = rb->mask };
	}
    }
#ifdef USE_LDXSTX
    while (UNLIKELY(stx32(&rb->tail, tail + actual, __ATOMIC_RELAXED)));
#else
    while (!__atomic_compare_exchange_n(&rb->tail,
					&tail,//Updated on failure
					tail + actual,
					/*weak=*/true,
					__ATOMIC_RELAXED,
					__ATOMIC_RELAXED));
#endif
    return (p64_ringbuf_result_t){ .index = tail, .actual = actual, .mask = rb->mask };
}

static inline void
release_slots(ringidx_t *loc,
	      ringidx_t idx,
	      int n,
	      bool loads_only,
	      uint32_t flags)
{
    if (flags & FLAG_MTSAFE)
    {
	//Wait for our turn to signal consumers (producers)
	if (UNLIKELY(__atomic_load_n(loc, __ATOMIC_RELAXED) != idx))
	{
	    SEVL();
	    while (WFE() && LDXR32(loc, __ATOMIC_RELAXED) != idx)
	    {
		DOZE();
	    }
	}
    }

    //Release elements to consumers (producers)
    //Also enable other producers (consumers) to proceed
    if (loads_only)
    {
	smp_fence(LoadStore);//Order loads only
	__atomic_store_n(loc, idx + n, __ATOMIC_RELAXED);
    }
    else
    {
#ifdef USE_DMB
	__atomic_thread_fence(__ATOMIC_RELEASE);//Order both loads and stores
	__atomic_store_n(loc, idx + n, __ATOMIC_RELAXED);
#else
	__atomic_store_n(loc, idx + n, __ATOMIC_RELEASE);
#endif
    }
}

inline p64_ringbuf_result_t
p64_ringbuf_acquire_(void *ptr,
		     uint32_t num,
		     bool enqueue)
{
    p64_ringbuf_t *rb = container_of(ptr, p64_ringbuf_t, ring);
    p64_ringbuf_result_t r;
    if (enqueue)
    {
	uint32_t mask = rb->prod.mask;
	uint32_t prod_flags = rb->prod.flags;

	if (!(prod_flags & FLAG_MTSAFE))
	{
	    //MT-unsafe single producer code
	    //Consumer metadata is swapped: cons.tail<->cons.head
	    r = acquire_slots(&rb->prod.head, &rb->cons.head/*cons.tail*/,
			      mask, num, true);
	}
	else
	{
	    //MT-safe multi producer code
	    r = acquire_slots_mtsafe(&rb->prod, num, true);
	}
    }
    else //dequeue
    {
	uint32_t mask = rb->cons.mask;
	uint32_t cons_flags = rb->cons.flags;

	if (rb->cons.flags & FLAG_LOCKFREE)
	{
	    //Use prod.head instead of cons.head (which is not used at all)
	    int actual;
	    //Speculative acquisition of slots
	    ringidx_t head = __atomic_load_n(&rb->prod.head, __ATOMIC_RELAXED);
            //Consumer metadata is swapped: cons.tail<->cons.head
            ringidx_t tail = __atomic_load_n(&rb->cons.head/*cons.tail*/,
                                             __ATOMIC_ACQUIRE);
            actual = MIN((int)num, (int)(tail - head));
            if (UNLIKELY(actual <= 0))
            {
		return (p64_ringbuf_result_t){ .index = 0, .actual = 0, .mask = mask };
            }
	    return (p64_ringbuf_result_t){ .index = head, .actual = actual, .mask = mask };

	}

	if (!(cons_flags & FLAG_MTSAFE))
	{
	    //MT-unsafe single consumer code
	    //Consumer metadata is swapped: cons.tail<->cons.head
	    r = acquire_slots(&rb->cons.head/*cons.tail*/, &rb->prod.head,
			      mask, num, false);
	}
	else
	{
	    //MT-safe multi consumer code
	    r = acquire_slots_mtsafe(&rb->cons, num, false);
	}
    }
    return r;
}

inline bool
p64_ringbuf_release_(void *ptr,
		     p64_ringbuf_result_t r,
		     bool enqueue)
{
    p64_ringbuf_t *rb = container_of(ptr, p64_ringbuf_t, ring);

    if (enqueue)
    {
	//Consumer metadata is swapped: cons.tail<->cons.head
	release_slots(&rb->cons.head/*cons.tail*/, r.index, r.actual,
		      /*loads_only=*/false, rb->prod.flags);
	return true;//Success
    }
    else //dequeue
    {
	if (rb->cons.flags & FLAG_LOCKFREE)
	{
	    bool success = __atomic_compare_exchange_n(&rb->prod.head,
						       &r.index,
						       r.index + r.actual,
						       /*weak=*/true,
						       __ATOMIC_RELEASE,
						       __ATOMIC_RELAXED);
	    return success;
	}
	release_slots(&rb->prod.head, r.index, r.actual,
		      /*loads_only=*/true, rb->cons.flags);
	return true;//Success
    }
}

//Enqueue elements at tail
UNROLL_LOOPS
uint32_t
p64_ringbuf_enqueue(p64_ringbuf_t *rb,
		    void *const *restrict ev,
		    uint32_t num)
{
    //Step 1: acquire slots
    uint32_t mask = rb->prod.mask;
    uint32_t prod_flags = rb->prod.flags;
    p64_ringbuf_result_t r;

    if (!(prod_flags & FLAG_MTSAFE))
    {
	//MT-unsafe single producer code
	//Consumer metadata is swapped: cons.tail<->cons.head
	r = acquire_slots(&rb->prod.head, &rb->cons.head/*cons.tail*/,
			  mask, num, true);
    }
    else
    {
	//MT-safe multi producer code
	r = acquire_slots_mtsafe(&rb->prod, num, true);
    }
    if (UNLIKELY(r.actual == 0))
    {
	return 0;
    }

    //Step 2: write slots
    for (uint32_t i = 0; i < r.actual; i++)
    {
	rb->ring[(r.index + i) & mask] = ev[i];
    }

    //Step 3: release slots to consumer
    //Consumer metadata is swapped: cons.tail<->cons.head
    release_slots(&rb->cons.head/*cons.tail*/, r.index, r.actual,
		  /*loads_only=*/false, prod_flags);

    return r.actual;
}

//Dequeue elements from head
UNROLL_LOOPS
uint32_t
p64_ringbuf_dequeue(p64_ringbuf_t *rb,
		    void **restrict ev,
		    uint32_t num,
		    uint32_t *index)
{
    uint32_t mask = rb->cons.mask;
    uint32_t cons_flags = rb->cons.flags;

    if (cons_flags & FLAG_LOCKFREE)
    {
	//Use prod.head instead of cons.head (which is not used at all)
	int actual;
	//Step 1: speculative acquisition of slots
	ringidx_t head = __atomic_load_n(&rb->prod.head, __ATOMIC_RELAXED);
	//Consumer metadata is swapped: cons.tail<->cons.head
	ringidx_t tail = __atomic_load_n(&rb->cons.head/*cons.tail*/,
					 __ATOMIC_ACQUIRE);
	do
	{
	    actual = MIN((int)num, (int)(tail - head));
	    if (UNLIKELY(actual <= 0))
	    {
		return 0;
	    }

	    //Step 2: read slots in advance (fortunately non-destructive)
	    for (uint32_t i = 0; i < (uint32_t)actual; i++)
	    {
		ev[i] = rb->ring[(head + i) & mask];
	    }

	    //Step 3: commit acquisition, release slots to producer
	}
	while (!__atomic_compare_exchange_n(&rb->prod.head,
					    &head,//Updated on failure
					    head + actual,
					    /*weak=*/true,
					    __ATOMIC_RELEASE,
					    __ATOMIC_RELAXED));
	*index = head;
	return actual;
    }

    //Step 1: acquire slots
    p64_ringbuf_result_t r;
    if (!(cons_flags & FLAG_MTSAFE))
    {
	//MT-unsafe single consumer code
	//Consumer metadata is swapped: cons.tail<->cons.head
	r = acquire_slots(&rb->cons.head/*cons.tail*/, &rb->prod.head,
			  mask, num, false);
    }
    else
    {
	//MT-safe multi consumer code
	r = acquire_slots_mtsafe(&rb->cons, num, false);
    }
    if (UNLIKELY(r.actual == 0))
    {
	return 0;
    }

    //Step 2: read slots
    for (uint32_t i = 0; i < r.actual; i++)
    {
	ev[i] = rb->ring[(r.index + i) & mask];
    }

    //Step 3: release slots to producer
    release_slots(&rb->prod.head, r.index, r.actual,
		  /*loads_only=*/true, cons_flags);

    *index = r.index;
    return r.actual;
}
