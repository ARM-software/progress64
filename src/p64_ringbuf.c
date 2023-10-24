//Copyright (c) 2017-2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include "p64_ringbuf.h"
#include "build_config.h"
#include "os_abstraction.h"

#include "arch.h"
#include "common.h"
#include "err_hnd.h"

#if defined USE_SPLIT_HEADTAIL && !defined USE_SPLIT_PRODCONS
#error USE_SPLIT_HEADTAIL not supported without USE_SPLIT_PRODCONS
#endif

#define SUPPORTED_FLAGS (P64_RINGBUF_F_SPENQ | P64_RINGBUF_F_MPENQ | \
			 P64_RINGBUF_F_SCDEQ | P64_RINGBUF_F_MCDEQ | \
			 P64_RINGBUF_F_NBENQ | P64_RINGBUF_F_NBDEQ | \
			 P64_RINGBUF_F_LFDEQ)

//0 means Single producer/consumer
#define FLAG_BLK      0x0001
#define FLAG_LOCKFREE 0x0002
#define FLAG_NONBLK   0x0004
#define FLAG_CLRSLOTS 0x0008

typedef uint32_t ringidx_t;
#define MAXELEMS 0xFFFFFFFF

struct idxpair
{
    ringidx_t cur;
    ringidx_t pend;
};

struct headtail
{
#if defined USE_SPLIT_PRODCONS
    struct idxpair head ALIGNED(CACHE_LINE);//tail for consumer
#else
    struct idxpair head;//tail for consumer
#endif
#if defined USE_SPLIT_HEADTAIL
    ringidx_t tail ALIGNED(CACHE_LINE);//head for consumer
#else
    ringidx_t tail;//head for consumer
#endif
    ringidx_t mask;
    ringidx_t capacity;
    uint32_t flags;
};

struct p64_ringbuf
{
    struct headtail prod;
    struct headtail cons;//head & tail are swapped for consumer metadata
    void *ring[] ALIGNED(CACHE_LINE);
} ALIGNED(CACHE_LINE);

p64_ringbuf_t *
p64_ringbuf_alloc(uint32_t nelems, uint32_t flags, size_t esize)
{
    if (nelems == 0 || nelems > MAXELEMS)
    {
	report_error("ringbuf", "invalid number of elements", nelems);
	return NULL;
    }
    //Can't specify both single-producer and non-blocking enqueue
    uint32_t invalid_combo0 = P64_RINGBUF_F_SPENQ | P64_RINGBUF_F_NBENQ;
    //Can't specify both single-consumer and non-blocking dequeue
    uint32_t invalid_combo1 = P64_RINGBUF_F_SCDEQ | P64_RINGBUF_F_NBDEQ;
    //Can't specify both single-consumer and lock-free dequeue
    uint32_t invalid_combo2 = P64_RINGBUF_F_SCDEQ | P64_RINGBUF_F_LFDEQ;
    //Can't mix lock-free dequeue with non-blocking enqueue
    uint32_t invalid_combo3 = P64_RINGBUF_F_LFDEQ | P64_RINGBUF_F_NBENQ;
    if ((flags & ~SUPPORTED_FLAGS) != 0 ||
	(flags & invalid_combo0) == invalid_combo0 ||
	(flags & invalid_combo1) == invalid_combo1 ||
	(flags & invalid_combo2) == invalid_combo2 ||
	(flags & invalid_combo3) == invalid_combo3 ||
	((flags & (P64_RINGBUF_F_NBENQ | P64_RINGBUF_F_NBDEQ)) != 0 && esize != sizeof(void *)))
    {
	report_error("ringbuf", "invalid flags", flags);
	return NULL;
    }
    uint64_t ringsz = ROUNDUP_POW2(nelems);
    size_t nbytes = sizeof(p64_ringbuf_t) + ringsz * esize;
    p64_ringbuf_t *rb = p64_malloc(nbytes, CACHE_LINE);
    if (rb != NULL)
    {
	rb->prod.head.cur = 0;
	rb->prod.head.pend = 0;
	rb->prod.tail = 0;
	rb->prod.mask = ringsz - 1;
	rb->prod.capacity = nelems;
	rb->prod.flags = (flags & P64_RINGBUF_F_SPENQ) ? 0 ://SPENQ
			 (flags & P64_RINGBUF_F_NBENQ) ? FLAG_NONBLK ://NBENQ
			 FLAG_BLK;//MPENQ
	rb->cons.head.cur = 0;
	rb->cons.head.pend = 0;
	rb->cons.tail = 0;
	rb->cons.mask = ringsz - 1;
	rb->cons.capacity = 0;//Not really used
	rb->cons.flags = (flags & P64_RINGBUF_F_SCDEQ) ? 0 ://SCDEQ
			 (flags & P64_RINGBUF_F_NBDEQ) ? FLAG_NONBLK ://NBDEQ
			 FLAG_BLK;//MCDEQ
	rb->cons.flags |= (flags & P64_RINGBUF_F_LFDEQ) ? FLAG_LOCKFREE : 0;
	//For non-blocking enqueue, we need to always clear dequeued slots
	rb->cons.flags |= (flags & P64_RINGBUF_F_NBENQ) ? FLAG_CLRSLOTS : 0;

	//Check for non-blocking enqueue or dequeue
	if (flags & (P64_RINGBUF_F_NBENQ | P64_RINGBUF_F_NBDEQ))
	{
	    //We need to initialise ring slots
	    for (uint32_t i = 0; i < ringsz; i++)
	    {
		rb->ring[i] = P64_RINGBUF_INVALID_ELEM;
	    }
	}
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
	if (rb->prod.head.cur != rb->cons.head/*tail*/.cur)
	{
	    report_error("ringbuf", "ring buffer not empty", rb);
	    return;
	}
	p64_mfree(rb);
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
	      ringidx_t capacity)
{
    ringidx_t tail = __atomic_load_n(tailp, __ATOMIC_RELAXED);
    ringidx_t head = __atomic_load_n(headp, __ATOMIC_ACQUIRE);
    int actual = MIN(n, (int)(capacity + head - tail));
    if (UNLIKELY(actual <= 0))
    {
	return (p64_ringbuf_result_t){ .index = 0, .actual = 0, .mask = 0 };
    }
    return (p64_ringbuf_result_t){ .index = tail, .actual = actual, .mask = mask };
}

//MT-safe multi producer/consumer code
static inline p64_ringbuf_result_t
acquire_slots_mtsafe(struct headtail *rb,
		     int n,
		     ringidx_t capacity)
{
    ringidx_t tail;
    int actual;
    tail = __atomic_load_n(&rb->tail, __ATOMIC_RELAXED);
    ringidx_t head = __atomic_load_n(&rb->head.cur, __ATOMIC_ACQUIRE);
    do
    {
	actual = MIN(n, (int)(capacity + head - tail));
	if (UNLIKELY(actual <= 0))
	{
	    return (p64_ringbuf_result_t){ .index = 0, .actual = 0, .mask = 0 };
	}
    }
    while (!__atomic_compare_exchange_n(&rb->tail,
					&tail,//Updated on failure
					tail + actual,
					/*weak=*/true,
					__ATOMIC_RELAXED,
					__ATOMIC_RELAXED));
    return (p64_ringbuf_result_t){ .index = tail,
				   .actual = actual,
				   .mask = rb->mask };
}

static inline void
release_slots(ringidx_t *loc,
	      ringidx_t idx,
	      uint32_t n,
	      bool loads_only,
	      uint32_t flags)
{
    if (flags & FLAG_BLK)
    {
	//Wait for our turn to signal consumers (producers)
	wait_until_equal(loc, idx, __ATOMIC_RELAXED);
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
	__atomic_store_n(loc, idx + n, __ATOMIC_RELEASE);
    }
}

#define USE_CACHED_LIMES

#define AHEAD(idx, n, cur) ((int)((idx) - (cur)) > 0)
#define INORDER(idx, n, cur) ((int)((idx) - (cur)) <= 0 && \
			      (int)((idx) + (n) - (cur)) > 0)
#define BEHIND(idx, n, cur) ((int)((idx) + (n) - (cur)) <= 0)
#ifdef USE_CACHED_LIMES
#define LIMES(pend, mask) ((pend) & (mask))
#define CHANGE(pend, mask) ((pend) & ~(mask))
#define ADD(a, b) ((a) + (b))
#endif

#if (__ATOMIC_RELAXED | __ATOMIC_ACQUIRE) != __ATOMIC_ACQUIRE
#error __ATOMIC bit-wise OR hack failed (see XXX)
#endif
#if (__ATOMIC_RELEASE | __ATOMIC_ACQUIRE) != __ATOMIC_RELEASE
#error __ATOMIC bit-wise OR hack failed (see XXX)
#endif

static inline struct idxpair
atomic_rb_release(struct headtail *rb,
		  ringidx_t idx,
		  uint32_t n,
		  int mo_load,
		  int mo_store)
{
    union
    {
	struct idxpair p;
	uint64_t u;
    } old, neu;
    __atomic_load(&rb->head, &old.p, mo_load);
    do
    {
#ifdef USE_CACHED_LIMES
	ringidx_t mask = rb->mask;//Shouldn't really load in LDX/STX section
	if (AHEAD(idx, n, old.p.cur))
	{
	    //Ahead: all elements will be released by some other thread
	    ringidx_t ring_size = mask + 1;
	    ringidx_t new_lim = idx + n - (old.p.cur + 1);
	    assert(new_lim > 0 && new_lim < ring_size);
	    neu.p.cur = old.p.cur;
	    //Increment change indicator, conditionally update 'limes' value
	    neu.p.pend = ADD(CHANGE(old.p.pend, mask), ring_size) |
			 MAX( LIMES(old.p.pend, mask), new_lim);
	    assert(LIMES(neu.p.pend, mask) != 0);
	    assert(neu.p.pend != old.p.pend);
	}
	else if (INORDER(idx, n, old.p.cur) && old.p.pend == 0)
	{
	    //In-order with no pending updates
	    //Release our updates, reset limes/change indicator
	    neu.p.cur = idx + n;
	    neu.p.pend = old.p.pend;//0
	    assert(neu.p.cur != old.p.cur);
	}
	else
	{
	    //In-order with pending updates or behind
	    //In-order: this thread is responsible for releasing any consecutive
	    //pending updates
	    //Behind: all elements have already been released
	    //Don't update 'loc' yet, that would enable other threads to release
	    return old.p;
	}
#else
	if (AHEAD(idx, n, old.p.cur))
	{
	    //Ahead: all elements will be released by some other thread
	    neu.p.cur = old.p.cur;
	    neu.p.pend = old.p.pend + 1;
	}
	else
	{
	    //In-order or behind
	    //In-order: this thread is responsible for releasing any consecutive
	    //pending updates
	    //Behind: all elements have already been released
	    //Don't update 'loc' yet, that would enable other threads to release
	    return old.p;
	}
#endif
    }
    while (!__atomic_compare_exchange(&rb->head,
				      &old.p,//Updated on failure
				      &neu.p,
				      /*weak=*/0,
				      mo_load | mo_store,//XXX
				      mo_load));
    return old.p;
}

static inline void
release_slots_nblk(struct headtail *rb,
		   ringidx_t idx,
		   uint32_t n,
		   ringidx_t *limit,
		   void **ring,
		   bool expect_null)
{
    struct idxpair old = atomic_rb_release(rb, idx, n,
					   __ATOMIC_ACQUIRE,
					   __ATOMIC_RELEASE);
#ifdef USE_CACHED_LIMES
    if (INORDER(idx, n, old.cur) && old.pend != 0)
#else
    if (INORDER(idx, n, old.cur))
#endif
    {
	ringidx_t mask = rb->mask;
	//In-order with pending updates that we must attempt to release
	struct idxpair neu;
	neu.cur = idx + n;//Skip past our own updates
	//Scan the ring searching for pending updates
	do
	{
#ifdef USE_CACHED_LIMES
	    ringidx_t limes = old.cur + 1 + LIMES(old.pend, mask);
	    assert(limes <= __atomic_load_n(limit, __ATOMIC_RELAXED));
	    (void)limit;
#else
	    ringidx_t limes = __atomic_load_n(limit, __ATOMIC_RELAXED);
#endif
	    while (neu.cur - old.cur < limes - old.cur)
	    {
		const void *elem = __atomic_load_n(&ring[neu.cur & mask],
						   __ATOMIC_ACQUIRE);
		if (expect_null ?
			elem != P64_RINGBUF_INVALID_ELEM :
			elem == P64_RINGBUF_INVALID_ELEM)
		{
		    //Didn't find expected value
		    break;
		}
		neu.cur++;
	    }
	    assert(neu.cur != old.cur);
#ifdef USE_CACHED_LIMES
	    //Since 'cur' changed, we need to recompute 'pend'
	    assert((int)(limes - neu.cur) >= 0);
	    if (neu.cur == limes)
	    {
		neu.pend = 0;
	    }
	    else//limes > neu.cur
	    {
		ringidx_t new_lim = limes - (neu.cur + 1);
		assert(new_lim > 0 && new_lim < mask + 1);
		neu.pend = new_lim;
	    }
#else
	    neu.pend = old.pend;
#endif
	}
	//Release our own updates + any found consecutive updates
	//Fail if 'loc->pend' has changed => more pending updates => re-scan
	//'loc->cur' cannot change
	while (!__atomic_compare_exchange(&rb->head,
					  &old,//Updated on failure
					  &neu,
					  /*weak=*/0,
					  __ATOMIC_ACQ_REL,
					  __ATOMIC_ACQUIRE));
    }
    //Else ahead or behind: do nothing
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

	if (!(prod_flags & (FLAG_BLK | FLAG_NONBLK)))
	{
	    //MT-unsafe single producer code
	    //Consumer metadata is swapped: cons.tail<->cons.head
	    r = acquire_slots(&rb->prod.head.cur,
			      &rb->cons.head/*tail*/.cur,
			      mask, num, rb->prod.capacity);
	}
	else
	{
	    //MT-safe multi producer code
	    r = acquire_slots_mtsafe(&rb->prod, num, rb->prod.capacity);
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
	    ringidx_t head = __atomic_load_n(&rb->prod.head.cur,
					     __ATOMIC_RELAXED);
	    //Consumer metadata is swapped: cons.tail<->cons.head
	    ringidx_t tail = __atomic_load_n(&rb->cons.head/*tail*/.cur,
					     __ATOMIC_ACQUIRE);
	    actual = MIN((int)num, (int)(tail - head));
	    if (UNLIKELY(actual <= 0))
	    {
		return (p64_ringbuf_result_t){ .index = 0,
					       .actual = 0,
					       .mask = 0 };
	    }
	    return (p64_ringbuf_result_t){ .index = head,
					   .actual = actual,
					   .mask = mask };
	}

	if (!(cons_flags & (FLAG_BLK | FLAG_NONBLK)))
	{
	    //MT-unsafe single consumer code
	    //Consumer metadata is swapped: cons.tail<->cons.head
	    r = acquire_slots(&rb->cons.head/*tail*/.cur,
			      &rb->prod.head.cur,
			      mask, num, 0);
	}
	else
	{
	    //MT-safe multi consumer code
	    r = acquire_slots_mtsafe(&rb->cons, num, 0);
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
	release_slots(&rb->cons.head/*tail*/.cur, r.index, r.actual,
		      /*loads_only=*/false, rb->prod.flags);
	return true;//Success
    }
    else //dequeue
    {
	if (rb->cons.flags & FLAG_LOCKFREE)
	{
	    bool success = __atomic_compare_exchange_n(&rb->prod.head.cur,
						       &r.index,
						       r.index + r.actual,
						       /*weak=*/true,
						       __ATOMIC_RELEASE,
						       __ATOMIC_RELAXED);
	    return success;
	}
	release_slots(&rb->prod.head.cur, r.index, r.actual,
		      /*loads_only=*/true, rb->cons.flags);
	return true;//Success
    }
}

UNROLL_LOOPS
static inline void
copy_voidptr(void **restrict dst, void *const *restrict src, uint32_t num)
{
    for (uint32_t i = 0; i < num; i++)
    {
	dst[i] = src[i];
    }
}

static inline void
write_slots(void **restrict rbring,
	    void *const *restrict ev,
	    const p64_ringbuf_result_t r)
{
    void **restrict ring0 = &rbring[r.index & r.mask];
    if (LIKELY(r.actual <= 1))
    {
	ring0[0] = ev[0];
	return;
    }
    uint32_t seg0 = r.mask + 1 - (r.index & r.mask);
    if (LIKELY(r.actual <= seg0))
    {
	/* One contiguous range */
	assert((r.index & r.mask) + seg0 <= r.mask + 1);
	copy_voidptr(ring0, ev, r.actual);
    }
    else
    {
	/* Range wraps around end of ring => two subranges */
	assert(seg0 < r.actual);
	copy_voidptr(ring0, ev, seg0);
	copy_voidptr(rbring, ev + seg0, r.actual - seg0);
    }
}

//Enqueue elements at tail
uint32_t
p64_ringbuf_enqueue(p64_ringbuf_t *rb,
		    void *const *restrict ev,
		    uint32_t num)
{
    //Step 1: acquire slots
    uint32_t mask = rb->prod.mask;
    uint32_t prod_flags = rb->prod.flags;
    p64_ringbuf_result_t r;

    if (!(prod_flags & (FLAG_BLK | FLAG_NONBLK)))//SPENQ
    {
	//MT-unsafe single producer code
	//Consumer metadata is swapped: cons.tail<->cons.head
	r = acquire_slots(&rb->prod.head.cur,
			  &rb->cons.head/*tail*/.cur,
			  mask, num, rb->prod.capacity);
    }
    else//MPENQ or NBENQ
    {
	//MT-safe multi producer code
	r = acquire_slots_mtsafe(&rb->prod, num, rb->prod.capacity);
    }
    if (UNLIKELY(r.actual == 0))
    {
	return 0;
    }

    //Step 2: write slots
    if (prod_flags & FLAG_NONBLK)//NBENQ
    {
	for (uint32_t i = 1; i < r.actual; i++)
	{
	    __atomic_store_n(&rb->ring[(r.index + i) & mask],
			     ev[i], __ATOMIC_RELAXED);
	}
	__atomic_store_n(&rb->ring[(r.index + 0) & mask],
			 ev[0], __ATOMIC_RELEASE);
    }
    else//SPENQ or MPENQ
    {
	write_slots(rb->ring, ev, r);
    }

    //Step 3: release slots to consumer
    if (prod_flags & FLAG_NONBLK)//NBENQ
    {
	release_slots_nblk(&rb->cons, r.index, r.actual, &rb->prod.tail,
			   rb->ring, /*expect_null=*/false);
    }
    else//SPENQ or NBENQ
    {
	//Consumer metadata is swapped: cons.tail<->cons.head
	release_slots(&rb->cons.head/*tail*/.cur, r.index, r.actual,
		      /*loads_only=*/false, prod_flags);
    }

    return r.actual;
}

static inline void
read_slots(void *const *restrict rbring,
	   void **restrict ev,
	   const p64_ringbuf_result_t r)
{
    void *const *restrict ring0 = &rbring[r.index & r.mask];
    if (LIKELY(r.actual <= 1))
    {
	ev[0] = ring0[0];
	return;
    }
    uint32_t seg0 = r.mask + 1 - (r.index & r.mask);
    if (LIKELY(r.actual <= seg0))
    {
	/* One contiguous range */
	assert((r.index & r.mask) + seg0 <= r.mask + 1);
	copy_voidptr(ev, ring0, r.actual);
    }
    else
    {
	/* Range wraps around end of ring => two subranges */
	assert(seg0 < r.actual);
	copy_voidptr(ev, ring0, seg0);
	copy_voidptr(ev + seg0, rbring, r.actual - seg0);
    }
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
	ringidx_t head = __atomic_load_n(&rb->prod.head.cur, __ATOMIC_RELAXED);
	//Consumer metadata is swapped: cons.tail<->cons.head
	ringidx_t tail = __atomic_load_n(&rb->cons.head/*tail*/.cur,
					 __ATOMIC_ACQUIRE);
	do
	{
	    actual = MIN((int)num, (int)(tail - head));
	    if (UNLIKELY(actual <= 0))
	    {
		return 0;
	    }

	    //Step 2: read slots in advance (fortunately non-destructive)
	    p64_ringbuf_result_t r = { .index = head,
				       .actual = actual,
				       .mask = mask };
	    read_slots(rb->ring, ev, r);

	    //Step 3: commit acquisition, release slots to producer
	}
	while (!__atomic_compare_exchange_n(&rb->prod.head.cur,
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
    if (!(cons_flags & (FLAG_BLK | FLAG_NONBLK)))//SCDEQ
    {
	//MT-unsafe single consumer code
	//Consumer metadata is swapped: cons.tail<->cons.head
	r = acquire_slots(&rb->cons.head/*tail*/.cur,
			  &rb->prod.head.cur,
			  mask, num, 0);
    }
    else//MCDEQ or NBDEQ
    {
	//MT-safe multi consumer code
	r = acquire_slots_mtsafe(&rb->cons, num, 0);
    }
    if (UNLIKELY(r.actual == 0))
    {
	return 0;
    }

    //Step 2: read slots
    if (cons_flags & (FLAG_NONBLK | FLAG_CLRSLOTS))//NBDEQ or NBENQ
    {
	for (uint32_t i = 0; i < r.actual; i++)
	{
	    void **slot = &rb->ring[(r.index + i) & mask];
	    ev[i] = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
	    assert(ev[i] != P64_RINGBUF_INVALID_ELEM);
	    __atomic_store_n(slot, P64_RINGBUF_INVALID_ELEM, __ATOMIC_RELAXED);
	}
    }
    else//SCDEQ or MCDEQ
    {
	read_slots(rb->ring, ev, r);
    }

    //Step 3: release slots to producer
    if (cons_flags & FLAG_NONBLK)//NBDEQ
    {
	release_slots_nblk(&rb->prod, r.index, r.actual, &rb->cons.tail/*head*/,
			   rb->ring, /*expect_null=*/true);
    }
    else//SCDEQ or MCDEQ
    {
	release_slots(&rb->prod.head.cur, r.index, r.actual,
		      /*loads_only=*/true, cons_flags);
    }

    *index = r.index;
    return r.actual;
}
