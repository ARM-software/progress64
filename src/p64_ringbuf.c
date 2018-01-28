//Copyright (c) 2017, ARM Limited. All rights reserved.
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
};

struct p64_ringbuf
{
    struct headtail prod;
    struct headtail cons;//NB head & tail are swapped for consumer metadata
    p64_element ring[0] ALIGNED(CACHE_LINE);
} ALIGNED(CACHE_LINE);

struct p64_ringbuf *p64_ringbuf_alloc(uint32_t ringsz)
{
    if (!IS_POWER_OF_TWO(ringsz))
    {
	fprintf(stderr, "Invalid ring size %u\n", ringsz), abort();
    }
    size_t nbytes = sizeof(struct p64_ringbuf) + ringsz * sizeof(p64_element);
    nbytes = (nbytes + CACHE_LINE - 1) & (CACHE_LINE - 1);
    struct p64_ringbuf *rb = aligned_alloc(CACHE_LINE, nbytes);
    if (rb != NULL)
    {
	rb->prod.head = 0;
	rb->prod.tail = 0;
	rb->prod.mask = ringsz - 1;
	rb->cons.head = 0;
	rb->cons.tail = 0;
	rb->cons.mask = ringsz - 1;
	return rb;
    }
    return NULL;
}

bool p64_ringbuf_free(struct p64_ringbuf *rb)
{
    if (rb->prod.head == rb->prod.tail && rb->cons.head == rb->cons.tail)
    {
	free(rb);
	return true;
    }
    return false;
}

struct result
{
    int n;//First (LSW) as this will be used to test for success
    ringidx_t idx;
};

static inline struct result atomic_rb_acquire(struct headtail *rb,
					      int n,
					      bool enqueue,
					      int mo_load,
					      int mo_store)
{
    ringidx_t old_top;
    ringidx_t ring_size = enqueue ? /*enqueue*/rb->mask + 1 : /*dequeue*/0;
    int actual;
#ifndef USE_LDXSTX
    old_top = rb->tail;
#endif
    do
    {
	ringidx_t bottom = __atomic_load_n(&rb->head, mo_load);
#ifdef USE_LDXSTX
	old_top = ldx32(&rb->tail, __ATOMIC_RELAXED);
#endif
	actual = MIN(n, (int)(ring_size + bottom - old_top));
	if (UNLIKELY(actual <= 0))
	{
	    return (struct result){ .idx = 0, .n = 0};
	}
    }
#ifdef USE_LDXSTX
    while (UNLIKELY(stx32(&rb->tail, old_top + actual, mo_store)));
#else
    while (!__atomic_compare_exchange_n(&rb->tail,
					&old_top,//Updated on failure
					old_top + actual,
					/*weak=*/true,
				        mo_store,
				        __ATOMIC_RELAXED));
#endif
    return (struct result){ .idx = old_top, .n = actual};
}

static inline void release_slots_blk(ringidx_t *loc,
				     ringidx_t idx,
				     int n,
				     bool loads_only)
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

    //Release elements to consumers (producers)
    //Also enable other producers (consumers) to proceed
    if (loads_only)
    {
	SMP_RMB();//Order loads only
	__atomic_store_n(loc, idx + n, __ATOMIC_RELAXED);
    }
    else
    {
#ifdef USE_DMB
	SMP_MB();//Order both loads and stores
	__atomic_store_n(loc, idx + n, __ATOMIC_RELAXED);
#else
	__atomic_store_n(loc, idx + n, __ATOMIC_RELEASE);
#endif
    }
}

int p64_ringbuf_enq(struct p64_ringbuf *rb,
		    const p64_element ev[],
		    int num)
{
    //Step 1: acquire slots
    uint32_t mask = rb->prod.mask;
    struct result r = atomic_rb_acquire(&rb->prod, num, true,
					__ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
    int actual = r.n;
    if (UNLIKELY(actual <= 0))
    {
	return 0;
    }

    //Step 2: access (write elements to) ring array
    ringidx_t old_tail = r.idx;
    ringidx_t new_tail = old_tail + actual;
    const p64_element *evp = ev;
    do
    {
	rb->ring[old_tail & mask] = *evp++;
    }
    while (++old_tail != new_tail);
    old_tail -= actual;

    //Step 3: release slots
    release_slots_blk(&rb->cons.head/*cons_tail*/, old_tail, actual,
		      /*loads_only=*/false);

    return actual;
}

int p64_ringbuf_deq(struct p64_ringbuf *rb,
		    p64_element ev[],
		    int num)
{
    //Step 1: acquire slots
    uint32_t mask = rb->cons.mask;
    struct result r = atomic_rb_acquire(&rb->cons, num, false,
					__ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
    int actual = r.n;
    if (UNLIKELY(actual <= 0))
    {
	return 0;
    }

    //Step 2: access (read elements from) ring array
    ringidx_t old_head = r.idx;
    ringidx_t new_head = old_head + actual;
    p64_element *evp = ev;
    do
    {
	p64_element elem = rb->ring[old_head & mask];
	PREFETCH_FOR_READ(elem);
	*evp++ = elem;
    }
    while (++old_head != new_head);
    old_head -= actual;

    //Step 3: release slots
    release_slots_blk(&rb->prod.head, old_head, actual, /*loads_only=*/true);

    return actual;
}
