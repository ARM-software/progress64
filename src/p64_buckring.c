//Copyright (c) 2019-2024, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause
//

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "p64_buckring.h"
#include "build_config.h"
#include "os_abstraction.h"

#include "arch.h"
#include "common.h"
#include "err_hnd.h"
#include "atomic.h"
#include "../src/lockfree.h" //For MO_LOAD()

//Use atomic_fetch_and/atomic_fetch_or varient instead of atomic_blend variant
//Performance is similar but atomic_blend variant perhaps slightly faster
//#define ATOMIC_AND_OR

//Use uintptr_t for ring slots instead of void ptr
//This will simplify bitwise operations on slot values and elements
#define NIL	   (uintptr_t)0
//Use 2 lsb for enqueue and dequeue in-order marks
#define DEQ_IOMARK (uintptr_t)1
#ifdef ATOMIC_AND_OR
//The enqueue out-of-order mark is the inverse of the in-order mark
#define ENQ_OOMARK (uintptr_t)2
#define IOMARKS (ENQ_OOMARK | DEQ_IOMARK)
#else //ATOMIC_BLEND
//The enqueue in-order mark is normal (2 => mark is set)
#define ENQ_IOMARK (uintptr_t)2
#define IOMARKS (ENQ_IOMARK | DEQ_IOMARK)
#endif

typedef uint32_t ringidx_t;

struct headtail
{
    ringidx_t head;
    ringidx_t hmask;
_Alignas(CACHE_LINE)
    ringidx_t tail;
    ringidx_t tmask;
};
//sizeof(struct headtail) == 2 * CACHE_LINE

struct p64_buckring
{
_Alignas(CACHE_LINE)
    struct headtail prod;
_Alignas(CACHE_LINE)
    struct headtail cons;
_Alignas(CACHE_LINE)
    uintptr_t ring[];
};
//sizeof(struct p64_buckring) == 4 * CACHE_LINE

//Number of elements per cache line
#define NELEM_PER_CL (CACHE_LINE / sizeof(uintptr_t))

static inline uint32_t
swizzle(uint32_t x)
{
    x = x ^ (x & (NELEM_PER_CL - 1)) << LOG2_32BIT(NELEM_PER_CL);
    return x;
}
#define SW(x) swizzle((x))

p64_buckring_t *
p64_buckring_alloc(uint32_t nelems, uint32_t flags)
{
    if (nelems < 1 || nelems > 0x80000000)
    {
	report_error("buckring", "invalid number of elements", nelems);
	return NULL;
    }
    if (flags != 0)
    {
	report_error("buckring", "invalid flags", nelems);
	return NULL;
    }
    size_t ringsize = ROUNDUP_POW2(nelems);
    size_t nbytes = ROUNDUP(sizeof(p64_buckring_t) +
			    ringsize * sizeof(uintptr_t),
			    CACHE_LINE);
    p64_buckring_t *rb = p64_malloc(nbytes, CACHE_LINE);
    if (rb != NULL)
    {
	//Initialise metadata
	memset(rb, 0, offsetof(p64_buckring_t, ring));
	rb->prod.head = 0;
	rb->prod.hmask = ringsize - 1;
	rb->prod.tail = 0;
	rb->prod.tmask = ringsize - 1;
	rb->cons.head = 0;
	rb->cons.hmask = ringsize - 1;
	rb->cons.tail = 0;
	rb->cons.tmask = ringsize - 1;
	//Initialise the ring pointers
#ifdef ATOMIC_AND_OR
	rb->ring[SW(0)] = NIL | DEQ_IOMARK;
#else
	rb->ring[SW(0)] = NIL | ENQ_IOMARK | DEQ_IOMARK;
#endif
	for (uint32_t i = 1; i < ringsize; i++)
	{
#ifdef ATOMIC_AND_OR
	    rb->ring[SW(i)] = NIL | ENQ_OOMARK;
#else
	    rb->ring[SW(i)] = NIL;
#endif
	}
	return rb;
    }
    return NULL;
}

void
p64_buckring_free(p64_buckring_t *rb)
{
    if (rb != NULL)
    {
	if (rb->prod.head != rb->prod.tail ||
	    rb->cons.head != rb->cons.tail)
	{
	    report_error("buckring", "ring buffer not empty", rb);
	    return;
	}
	p64_mfree(rb);
    }
}

struct result
{
    ringidx_t index;
    uint32_t actual;
};

//Acquire slots in a ring buffer
//read_ptr - head for producer / tail for consumer
//write_ptr - tail for producer / head for consumer
//ring_size - mask + 1 for producer / 0 for consumer
static inline struct result
atomic_rb_acquire(ringidx_t *read_ptr,
		  ringidx_t *write_ptr,
		  bool enqueue,
		  int n)
{
    ringidx_t tail, mask;
    int actual;
#ifdef __ARM_FEATURE_ATOMICS
    tail = atomic_icas_n(write_ptr, __ATOMIC_RELAXED);
#else
    tail = atomic_load_n(write_ptr, __ATOMIC_RELAXED);
#endif
    mask = *(read_ptr + 1);//Mask is located after head/tail
    ringidx_t ring_size = enqueue ? mask + 1 : 0;
    do
    {
	ringidx_t head = atomic_load_n(read_ptr, __ATOMIC_ACQUIRE);
	actual = MIN(n, (int)(ring_size + head - tail));
	if (UNLIKELY(actual <= 0))
	{
	    return (struct result){ .index = 0, .actual = 0 };
	}
    }
    while (!atomic_compare_exchange_n(write_ptr,
				      &tail,//Updated on failure
				      tail + actual,
				      __ATOMIC_RELAXED,
				      __ATOMIC_RELAXED));
    return (struct result){ .index = tail, .actual = actual };
}

#ifndef ATOMIC_AND_OR
//Atomic blend operation
//Blend old and new value based on the blend mask
static inline uintptr_t
atomic_blend(uintptr_t *loc,
	     uintptr_t new_val,
	     uintptr_t preserve_mask,
	     int mm)
{
    uintptr_t old, neu;
#ifdef __ARM_FEATURE_ATOMICS
    old = atomic_icas_n(loc, __ATOMIC_RELAXED);
#else
    old = atomic_load_n(loc, __ATOMIC_RELAXED);
#endif
    do
    {
	//Compute blended value from new_val and kept bits of old value
	neu = (new_val & ~preserve_mask) | (old & preserve_mask);
    }
    while (UNLIKELY(!atomic_compare_exchange_n(loc,
					       &old,//Updated on failure
					       neu,
					       mm,
					       MO_LOAD(mm))));
    return old;
}
#endif

static inline uint32_t
enq_deq(p64_buckring_t *rb,
	void *ev[],
	uint32_t num,
	uint32_t *idx_ptr,
	bool enqueue)
{
    //Step 1: acquire slots
    const struct result r =
	atomic_rb_acquire(enqueue ? &rb->prod.head : &rb->cons.tail,//read
			  enqueue ? &rb->prod.tail : &rb->cons.head,//write
			  enqueue,
			  num);
    if (UNLIKELY(r.actual == 0))
    {
	return 0;
    }
    if (idx_ptr != NULL)
    {
	//Return the starting ring index
	*idx_ptr = r.index;
    }

    //Step 2: Write slots (write NIL for dequeue)
    ringidx_t mask = enqueue ? rb->prod.hmask : rb->cons.tmask;
    uintptr_t old;
    if (enqueue)
    {
	//Write all but first slot
	for (uint32_t i = 1; i < r.actual; i++)
	{
	    assert(rb->ring[SW(r.index + i) & mask] == NIL);
	    uintptr_t elem = (uintptr_t)ev[i];
	    if (UNLIKELY((elem & IOMARKS) != 0 || elem == NIL))
	    {
		report_error("buckring", "invalid element pointer", elem);
		abort();
	    }
#ifdef ATOMIC_AND_OR
	    rb->ring[SW(r.index + i) & mask] = elem | ENQ_OOMARK;
#else
	    rb->ring[SW(r.index + i) & mask] = elem;
#endif
	}
	//Write first slot last
	//Preserve dequeue iomark, clear enqueue iomark
	//Release our elements, acquire any later elements
	uintptr_t elem = (uintptr_t)ev[0];
	if (UNLIKELY((elem & IOMARKS) != 0 || elem == NIL))
	{
	    report_error("buckring", "invalid element pointer", elem);
	    abort();
	}
#ifdef ATOMIC_AND_OR
	old = atomic_fetch_or(&rb->ring[SW(r.index) & mask],
			        elem | //new value
				 ENQ_OOMARK,//set enqueue out-of-order mark
				 __ATOMIC_ACQ_REL);
#else
	old = atomic_blend(&rb->ring[SW(r.index) & mask],
			   elem,//new value
			   DEQ_IOMARK,//preserve DEQ_IOMARK, clear ENQ_IOMARK
			   __ATOMIC_ACQ_REL);
#endif
    }
    else//dequeue
    {
	//For dequeue we also save the old values of the slots
	//Write (clear) all but first slot
	for (uint32_t i = 1; i < r.actual; i++)
	{
	    //TODO investigate whether using atomic_exchange_n() is beneficial
	    old = rb->ring[SW(r.index + i) & mask];
	    rb->ring[SW(r.index + i) & mask] = NIL;
	    ev[i] = (void *)(old & ~IOMARKS);//Can iomarks actually be present?
	    assert(ev[i] != NIL);
	}
	//Write (clear) first slot last
	//Preserve enqueue iomark, clear dequeue iomark
	//Release our elements, acquire any later elements
#ifdef ATOMIC_AND_OR
	old = atomic_fetch_and(&rb->ring[SW(r.index) & mask],
			       ENQ_OOMARK,
			       __ATOMIC_ACQ_REL);
#else
	old = atomic_blend(&rb->ring[SW(r.index) & mask],
			   NIL,//new value
			   ENQ_IOMARK,
			   __ATOMIC_ACQ_REL);
#endif
	ev[0] = (void *)(old & ~IOMARKS);
	assert(ev[0] != NIL);
    }

    //Check if we released out-of-order
    //Enqueue: if ENQ_IOMARK not present then out-of-order else in-order
    //Dequeue: if DEQ_IOMARK not present then out-of-order else in-order
#ifdef ATOMIC_AND_OR
    if (enqueue ? (old & ENQ_OOMARK) != 0 : (old & DEQ_IOMARK) == 0)
#else
    if (enqueue ? (old & ENQ_IOMARK) == 0 : (old & DEQ_IOMARK) == 0)
#endif
    {
	//Specific IOMARK not present => out-of-order, we are done
	return r.actual;
    }
    //Else we have been PASSED THE BUCK
    //We are now in-order and responsible for finding consecutive in-order slots
    //that can also be released
    ringidx_t index = r.index + 1;//Continue with next slot
    uintptr_t new;
    do
    {
	//Seems faster to re-load here inside the while-loop even as
	//atomic_compare_exchange_n() updated 'old'
#ifdef __ARM_FEATURE_ATOMICS
	old = atomic_icas_n(&rb->ring[SW(index) & mask], __ATOMIC_RELAXED);
#else
	old = atomic_load_n(&rb->ring[SW(index) & mask], __ATOMIC_RELAXED);
#endif
	uintptr_t elem = old & ~IOMARKS;
	while (enqueue ?
		/*enqueue*/elem != NIL && (old & DEQ_IOMARK) == 0 :
#ifdef ATOMIC_AND_OR
		/*dequeue*/elem == NIL && (old & ENQ_OOMARK) != 0
#else
		/*dequeue*/elem == NIL && (old & ENQ_IOMARK) == 0
#endif
	      )
	{
	    index++;//One more match, continue with next slot
	    old = atomic_load_n(&rb->ring[SW(index) & mask],
				   __ATOMIC_RELAXED);
	    elem = old & ~IOMARKS;
	}
	//End of matching slots
	//Enqueue: found NIL or dequeue iomark
	//Dequeue: found element or enqueue iomark
	//Mark next slot as in order - PASS THE BUCK
	//Set our iomark, preserve all other bits
	if (enqueue)
	{
#ifdef ATOMIC_AND_OR
	    new = old & ~ENQ_OOMARK;//Clear enqueue out-of-order mark
#else
	    new = old | ENQ_IOMARK;//Set enqueue in-order mark
#endif
	}
	else
	{
	    new = old | DEQ_IOMARK;//Set dequeue iomark
	}
    }
    while (!atomic_compare_exchange_n(&rb->ring[SW(index) & mask],
				      &old,//Updated on failure
				      new,
				      __ATOMIC_ACQ_REL,
				      __ATOMIC_ACQUIRE));

    //Finally make all released slots available for new acquisitions
    atomic_fetch_add(enqueue ? &rb->cons.tail : &rb->prod.head,
		     index - r.index, __ATOMIC_RELEASE);
    return r.actual;
}

uint32_t
p64_buckring_enqueue(p64_buckring_t *rb,
		     void *const ev[],
		     uint32_t num)
{
    return enq_deq(rb, (void **)ev, num, NULL, true);
}

uint32_t
p64_buckring_dequeue(p64_buckring_t *rb,
		     void *ev[],
		     uint32_t num,
		     uint32_t *index)
{
    return enq_deq(rb, ev, num, index, false);
}
