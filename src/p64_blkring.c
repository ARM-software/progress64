//Copyright (c) 2024, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <assert.h>

#include "p64_blkring.h"

#include "arch.h"
#include "lockfree.h"
#include "common.h"
#include "err_hnd.h"
#include "build_config.h"
#include "os_abstraction.h"

#define MAXELEMS 0x80000000

static inline uint32_t
swizzle(uint32_t sn)
{
    //Compute index to array slot but consecutive sequence
    //numbers will be located in different cache lines
#if CACHE_LINE == 64
    //64B cache line and 16B (128-bit) array elements =>
    //4 elements/cache line
    sn = sn ^ (sn & 3) << 2;
#define SWIZZLE_BITS 4
#else
    //Assume 128B cache line => 8 elements/cache line
    sn = sn ^ (sn & 7) << 3;
#define SWIZZLE_BITS 6
#endif
    return sn;
}

struct ringslot
{
    union
    {
	struct
	{
	    uintptr_t sn;
	    void *elem;
	};
	__int128 i128;
    };
};

typedef struct p64_blkring
{
    _Alignas(CACHE_LINE)
    struct
    {
	uint32_t head;
	uint32_t mask;
    } cons;
    _Alignas(CACHE_LINE)
    struct
    {
	uint32_t tail;
	uint32_t mask;
    } prod;
    _Alignas(CACHE_LINE)
    struct ringslot ring[];
} p64_blkring_t;

p64_blkring_t *
p64_blkring_alloc(uint32_t nelems)
{
    if (nelems == 0 || nelems > MAXELEMS)
    {
	report_error("blkring", "invalid number of elements", nelems);
	return NULL;
    }
    uint32_t p2size = ROUNDUP_POW2(nelems);
    if (p2size < (1U << SWIZZLE_BITS))
    {
	//Minimum size required by swizzle()
	p2size = 1U << SWIZZLE_BITS;
    }
    p64_blkring_t *rb = p64_malloc(sizeof(p64_blkring_t) + p2size * sizeof(struct ringslot), CACHE_LINE);
    if (rb == NULL)
    {
	return NULL;
    }
    rb->cons.head = 0;
    rb->cons.mask = p2size - 1;
    rb->prod.tail = 0;
    rb->prod.mask = p2size - 1;
    for (uint32_t i = 0; i < p2size; i++)
    {
	uint32_t j = swizzle(i);
	assert(j < p2size);
	rb->ring[j].sn = i;
	rb->ring[j].elem = NULL;
    }
    return rb;
}

void
p64_blkring_free(p64_blkring_t *rb)
{
    int32_t dif = rb->prod.tail - rb->cons.head;
    if (dif < 0)
    {
	report_error("blkring", "blocking ring buffer has waiting consumers", -dif);
    }
    else if (dif > 0)
    {
	report_error("blkring", "blocking ring buffer not empty", dif);
    }
    p64_mfree(rb);
}

void
p64_blkring_enqueue(p64_blkring_t *rb, void *const elems[], uint32_t nelem)
{
    uint32_t sn = __atomic_fetch_add(&rb->prod.tail, nelem, __ATOMIC_RELAXED);
    uint32_t mask = *addr_dep(&rb->prod.mask, sn);
    for (uint32_t i = 0; i < nelem; i++, sn++)
    {
	//NULL elements are not allowed
	if (UNLIKELY(elems[i] == NULL))
	{
	    abort();
	}
	uint32_t idx = swizzle(sn) & mask;
#ifndef __ARM_FEATURE_ATOMICS
	struct ringslot old;
	//Wait for slot to become empty, then write our element into it
	do
	{
#ifdef __ARM_FEATURE_ATOMICS
	    //Atomic read using ICAS
	    old.i128 = casp(&rb->ring[idx].i128, 0, 0, __ATOMIC_ACQUIRE);
#else
	    //Prefetch for write as we will write later
	    PREFETCH_FOR_WRITE(&rb->ring[idx]);
	    //Non-atomic read, ensure 'elem' is read after 'sn'
	    old.sn = __atomic_load_n(&rb->ring[idx].sn, __ATOMIC_RELAXED);
	    old.elem = __atomic_load_n(addr_dep(&rb->ring[idx].elem, old.sn), __ATOMIC_RELAXED);
#endif
	}
	while (old.sn != sn || old.elem != NULL);
	//Now write our new element
	__atomic_store_n(&rb->ring[idx].elem, elems[i], __ATOMIC_RELEASE);
#else
	//If slot is empty then atomically write our element into it
	struct ringslot cmp, swp;
	cmp.sn = sn;
	cmp.elem = NULL;
	swp.sn = sn;
	swp.elem = elems[i];
	while (casp(&rb->ring[idx].i128, cmp.i128, swp.i128, __ATOMIC_RELEASE) != cmp.i128)
	{
	    //CAS comparison failed, no write
	    //Tail must have wrapped around (too small ring buffer or slow consumer)
	    //Cannot succeed until consumer has dequeued previous element in this slot
	    wait_until_equal((uintptr_t *)&rb->ring[idx].elem, (uintptr_t)NULL, __ATOMIC_RELAXED);
	}
#endif
    }
}

static void
blkring_dequeue(p64_blkring_t *rb, void *elems[], uint32_t nelem, uint32_t *index, uint32_t sn)
{
    uint32_t mask = *addr_dep(&rb->cons.mask, sn);
    *index = sn;
    for (uint32_t i = 0; i < nelem; i++, sn++)
    {
	uint32_t idx = swizzle(sn) & mask;
	//Wait for slot to become filled, then replace with NULL and updated seqno
	struct ringslot old;
	do
	{
#ifdef __ARM_FEATURE_ATOMICS
	    //Atomic read using ICAS
	    old.i128 = casp(&rb->ring[idx].i128, 0, 0, __ATOMIC_ACQUIRE);
#else
	    //Prefetch for write as we will write later
	    PREFETCH_FOR_WRITE(&rb->ring[idx]);
	    //Non-atomic read, ensure 'elem' is read after 'sn'
	    old.sn = __atomic_load_n(&rb->ring[idx].sn, __ATOMIC_RELAXED);
	    old.elem = __atomic_load_n(addr_dep(&rb->ring[idx].elem, old.sn), __ATOMIC_ACQUIRE);
#endif
	}
	while (old.sn != sn || old.elem == NULL);
#ifdef __ARM_FEATURE_ATOMICS
	//Atomic write
	struct ringslot swp;
	swp.sn = sn + mask + 1;
	swp.elem = NULL;
	//CAS cannot fail
	if (UNLIKELY(casp(&rb->ring[idx].i128, old.i128, swp.i128, __ATOMIC_RELAXED) != old.i128))
	{
	    abort();
	}
#else
	//Non-atomic write, ensure 'sn' is written after 'elem'
	__atomic_store_n(&rb->ring[idx].elem, NULL, __ATOMIC_RELAXED);
	__atomic_store_n(&rb->ring[idx].sn, sn + mask + 1, __ATOMIC_RELEASE);
#endif
	assert(old.elem != NULL);
	elems[i] = old.elem;
    }
}

void
p64_blkring_dequeue(p64_blkring_t *rb, void *elems[], uint32_t nelem, uint32_t *index)
{
    uint32_t head = __atomic_fetch_add(&rb->cons.head, nelem, __ATOMIC_RELAXED);
    blkring_dequeue(rb, elems, nelem, index, head);
}

uint32_t
p64_blkring_dequeue_nblk(p64_blkring_t *rb, void *elems[], uint32_t nelem, uint32_t *index)
{
    uint32_t head = __atomic_load_n(&rb->cons.head, __ATOMIC_RELAXED);
    uint32_t num;
    do
    {
	//Always get a fresh copy of prod.tail
	//This access creates a shared copy of rb->prod which is bad for scalability
	uint32_t tail = __atomic_load_n(&rb->prod.tail, __ATOMIC_RELAXED);
	int32_t avail = tail - head;
	if (avail <= 0)
	{
	    return 0;
	}
	num = (uint32_t)avail < nelem ? (uint32_t)avail : nelem;
    }
    while (!__atomic_compare_exchange_n(&rb->cons.head, &head, head + num, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    //'head' updated on failure
    blkring_dequeue(rb, elems, num, index, head);
    return num;
}
