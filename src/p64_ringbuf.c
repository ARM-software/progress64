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

#define SUPPORTED_FLAGS (P64_RINGBUF_FLAG_SP | P64_RINGBUF_FLAG_MP | \
			 P64_RINGBUF_FLAG_SC | P64_RINGBUF_FLAG_MC)

#define FLAG_MTSAFE 0x0001

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
p64_ringbuf_alloc(uint32_t nelems, uint32_t flags)
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
    size_t nbytes = ROUNDUP(sizeof(p64_ringbuf_t) + ringsz * sizeof(void *),
			    CACHE_LINE);
    p64_ringbuf_t *rb = aligned_alloc(CACHE_LINE, nbytes);
    if (rb != NULL)
    {
	rb->prod.head = 0;
	rb->prod.tail = 0;
	rb->prod.mask = ringsz - 1;
	rb->prod.flags = (flags & P64_RINGBUF_FLAG_SP) ? 0 : FLAG_MTSAFE;
	rb->cons.head = 0;
	rb->cons.tail = 0;
	rb->cons.mask = ringsz - 1;
	rb->cons.flags = (flags & P64_RINGBUF_FLAG_SC) ? 0 : FLAG_MTSAFE;
	return rb;
    }
    return NULL;
}

void
p64_ringbuf_free(p64_ringbuf_t *rb)
{
    if (rb != NULL)
    {
	if (rb->prod.head != rb->prod.tail || rb->cons.head != rb->cons.tail)
	{
	    fprintf(stderr, "Ring buffer %p is not empty\n", rb), abort();
	}
	free(rb);
    }
}

struct result
{
    int n;//First (LSW) as this will be used to test for success
    ringidx_t idx;
};

static inline struct result
atomic_rb_acquire(struct headtail *rb,
		  int n,
		  bool enqueue,
		  int mo_load,
		  int mo_store)
{
    ringidx_t old_top;
    ringidx_t ring_size = enqueue ? /*enqueue*/rb->mask + 1 : /*dequeue*/0;
    int actual;
    if (!(rb->flags & FLAG_MTSAFE))
    {
	//MT-unsafe single producer/consumer code
	old_top = rb->tail;
	ringidx_t bottom = __atomic_load_n(&rb->head, mo_load);
	actual = MIN(n, (int)(ring_size + bottom - old_top));
	if (UNLIKELY(actual <= 0))
	{
	    return (struct result){ .idx = 0, .n = 0 };
	}
	rb->tail = old_top + actual;
	return (struct result){ .idx = old_top, .n = actual };
    }
    //MT-safe multi producer/consumer code
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
	    return (struct result){ .idx = 0, .n = 0 };
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
    return (struct result){ .idx = old_top, .n = actual };
}

static inline void
release_slots_blk(ringidx_t *loc,
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

uint32_t
p64_ringbuf_enqueue(p64_ringbuf_t *rb,
		    void *ev[],
		    uint32_t num)
{
    //Step 1: acquire slots
    uint32_t mask = rb->prod.mask;
    uint32_t prod_flags = rb->prod.flags;
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
    void **evp = ev;
    do
    {
	rb->ring[old_tail & mask] = *evp++;
    }
    while (++old_tail != new_tail);
    old_tail -= actual;

    //Step 3: release slots
    release_slots_blk(&rb->cons.head/*cons_tail*/, old_tail, actual,
		      /*loads_only=*/false, prod_flags);

    return actual;
}

uint32_t
p64_ringbuf_dequeue(p64_ringbuf_t *rb,
		    void *ev[],
		    uint32_t num)
{
    //Step 1: acquire slots
    uint32_t mask = rb->cons.mask;
    uint32_t cons_flags = rb->cons.flags;
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
    void **evp = ev;
    do
    {
	void *elem = rb->ring[old_head & mask];
	PREFETCH_FOR_READ(elem);
	*evp++ = elem;
    }
    while (++old_head != new_head);
    old_head -= actual;

    //Step 3: release slots
    release_slots_blk(&rb->prod.head, old_head, actual,
		      /*loads_only=*/true, cons_flags);

    return actual;
}
