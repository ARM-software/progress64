//Copyright (c) 2017-2025, ARM Limited. All rights reserved.
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
#include "atomic.h"

#define SUPPORTED_FLAGS (P64_RINGBUF_F_SPENQ | P64_RINGBUF_F_MPENQ | \
			 P64_RINGBUF_F_SCDEQ | P64_RINGBUF_F_MCDEQ | \
			 P64_RINGBUF_F_NBENQ | P64_RINGBUF_F_NBDEQ | \
			 P64_RINGBUF_F_LFDEQ)

//0 means Single producer/consumer
#define FLAG_BLK      0x0001
#define FLAG_LOCKFREE 0x0002
#define FLAG_NONBLK   0x0004
#define FLAG_MASK     0x0007

#define PROD_FLAGS(rb) ((((uintptr_t)rb)     ) & FLAG_MASK)
#define CONS_FLAGS(rb) ((((uintptr_t)rb) >> 3) & FLAG_MASK)
#define RB(rb) ((p64_ringbuf_t *)((uintptr_t)rb & ~0x3FUL))
//Need to ensure the ring buffer is at least 64-byte aligned
#if CACHE_LINE >= 64
#define RB_ALIGNMENT CACHE_LINE
#else
//Cache line size might be smaller on e.g. ARMv7
#define RB_ALIGNMENT 64
#endif

typedef uint32_t ringidx_t;
#define MAXELEMS 0xFFFFFFFF
#define PENDMAX 32

struct idxpair
{
    _Alignas(2 * sizeof(ringidx_t))
    ringidx_t cur;
    uint32_t pend;
};

union endpoint
{
    struct
    {
	_Alignas(4 * sizeof(ringidx_t))
	struct idxpair head;//tail for consumer
	ringidx_t tail;//head for consumer
	ringidx_t capacity;
    };
#ifdef __ARM_FEATURE_ATOMICS
    __int128 atom;
#endif
};

struct p64_ringbuf
{
    _Alignas(CACHE_LINE)
    union endpoint prod;
    ringidx_t prod_mask;//Mask must be directly after the endpoint member
#ifdef USE_SPLIT_PRODCONS
    _Alignas(CACHE_LINE)
#endif
    union endpoint cons;//head & tail are swapped for consumer metadata
    ringidx_t cons_mask;//Mask must be directly after the endpoint member
    _Alignas(CACHE_LINE)
    void *ring[];
};

p64_ringbuf_t *
p64_ringbuf_alloc(uint32_t nelems, uint32_t flags, size_t esize)
{
    if (nelems == 0 || nelems > MAXELEMS)
    {
	report_error("ringbuf", "invalid number of elements", nelems);
	return NULL;
    }
    //Can't specify both single-producer and MP non-blocking enqueue
    uint32_t invalid_combo0 = P64_RINGBUF_F_SPENQ | P64_RINGBUF_F_NBENQ;
    //Can't specify both single-consumer and MP non-blocking dequeue
    uint32_t invalid_combo1 = P64_RINGBUF_F_SCDEQ | P64_RINGBUF_F_NBDEQ;
    //Can't specify both single-consumer and MP lock-free dequeue
    uint32_t invalid_combo2 = P64_RINGBUF_F_SCDEQ | P64_RINGBUF_F_LFDEQ;
    //Can't specify both MP non-blocking dequeue and MP lock-free dequeue
    uint32_t invalid_combo3 = P64_RINGBUF_F_NBDEQ | P64_RINGBUF_F_LFDEQ;
    if ((flags & ~SUPPORTED_FLAGS) != 0 ||
	(flags & invalid_combo0) == invalid_combo0 ||
	(flags & invalid_combo1) == invalid_combo1 ||
	(flags & invalid_combo2) == invalid_combo2 ||
	(flags & invalid_combo3) == invalid_combo3)
    {
	report_error("ringbuf", "invalid flags", flags);
	return NULL;
    }
    uint64_t ringsz = ROUNDUP_POW2(nelems);
    size_t nbytes = sizeof(p64_ringbuf_t) + ringsz * esize;
    p64_ringbuf_t *rb = p64_malloc(nbytes, RB_ALIGNMENT);
    if (rb != NULL)
    {
	uint32_t prod_flags, cons_flags;
	rb->prod.head.cur = 0;
	rb->prod.head.pend = 0;
	rb->prod.tail = 0;
	rb->prod.capacity = nelems;
	rb->prod_mask = ringsz - 1;
	    prod_flags = (flags & P64_RINGBUF_F_SPENQ) ? 0 ://SPENQ
			 (flags & P64_RINGBUF_F_NBENQ) ? FLAG_NONBLK ://NBENQ
			 FLAG_BLK;//MPENQ
	rb->cons.head.cur = 0;
	rb->cons.head.pend = 0;
	rb->cons.tail = 0;
	rb->cons.capacity = 0;
	rb->cons_mask = ringsz - 1;
	    cons_flags = (flags & P64_RINGBUF_F_SCDEQ) ? 0 ://SCDEQ
			 (flags & P64_RINGBUF_F_NBDEQ) ? FLAG_NONBLK ://NBDEQ
			 FLAG_BLK;//MCDEQ
	    cons_flags |= (flags & P64_RINGBUF_F_LFDEQ) ? FLAG_LOCKFREE : 0;
	rb = (p64_ringbuf_t *)((uintptr_t)rb | (cons_flags << 3) | prod_flags);
	return rb;
    }
    return NULL;
}

p64_ringbuf_t *
p64_ringbuf_alloc_(uint32_t nelems, uint32_t flags, size_t esize)
{
    p64_ringbuf_t *rb = p64_ringbuf_alloc(nelems, flags, esize);
    return rb;
}

void
p64_ringbuf_free(p64_ringbuf_t *rb)
{
    if (rb != NULL)
    {
	rb = RB(rb);
	if (rb->prod.head.cur != rb->cons.head/*tail*/.cur)
	{
	    report_error("ringbuf", "ring buffer not empty", rb);
	    return;
	}
	p64_mfree(rb);
    }
}

void
p64_ringbuf_free_(p64_ringbuf_t *rb)
{
    p64_ringbuf_free(rb);
}

//MT-unsafe single producer/consumer code
static inline p64_ringbuf_result_t
acquire_slots(ringidx_t *headp,
	      ringidx_t *tailp,
	      ringidx_t mask,
	      int n,
	      ringidx_t capacity)
{
    ringidx_t tail = atomic_load_n(tailp, __ATOMIC_RELAXED);
    ringidx_t head = atomic_load_n(headp, __ATOMIC_ACQUIRE);
    int actual = MIN(n, (int)(capacity + head - tail));
    if (UNLIKELY(actual <= 0))
    {
	return (p64_ringbuf_result_t){ .index = 0, .actual = 0, .mask = 0 };
    }
    return (p64_ringbuf_result_t){ .index = tail, .actual = actual, .mask = mask };
}

//MT-safe multi producer/consumer code
static inline p64_ringbuf_result_t
acquire_slots_mtsafe(union endpoint *ep,
		     int n)
{
    ringidx_t head, tail, capacity;
    uint32_t mask;
    int actual;
#ifdef __ARM_FEATURE_ATOMICS
    union endpoint mem, swp, cmp;
    //Atomic read using ICAS, preparing for ensuing write
    mem.atom = atomic_icas_n(&ep->atom, __ATOMIC_RELAXED);
    //Address dependency to prevent accesses through 'ep' to be speculated before the ICAS above
    ep = addr_dep(ep, mem.atom);
    capacity = mem.capacity;
#else
    tail = atomic_load_n(&ep->tail, __ATOMIC_RELAXED);
    head = atomic_load_n(&ep->head.cur, __ATOMIC_ACQUIRE);
    capacity = ep->capacity;
#endif
    //Mask is located imediately after the endpoint
    static_assert(offsetof(struct p64_ringbuf, prod_mask) == offsetof(struct p64_ringbuf, prod) + sizeof(union endpoint), "prod_mask");
    static_assert(offsetof(struct p64_ringbuf, cons_mask) == offsetof(struct p64_ringbuf, cons) + sizeof(union endpoint), "cons_mask");
    mask = *(uint32_t *)(ep + 1);
    do
    {
#ifdef __ARM_FEATURE_ATOMICS
	tail = mem.tail;
	head = mem.head.cur;
#endif
	actual = MIN(n, (int)(capacity + head - tail));
	if (UNLIKELY(actual <= 0))
	{
	    return (p64_ringbuf_result_t){ .index = 0, .actual = 0, .mask = 0 };
	}
#ifdef __ARM_FEATURE_ATOMICS
	cmp = swp = mem;
	swp.tail += actual;
	mem.atom = atomic_cas_n(&ep->atom, cmp.atom, swp.atom, __ATOMIC_ACQUIRE);//Load-acquire head
#endif
    }
#ifdef __ARM_FEATURE_ATOMICS
    while (UNLIKELY(mem.atom != cmp.atom));
#else
    while (!atomic_compare_exchange_n(&ep->tail,
				      &tail,//Updated on failure
				      tail + actual,
				      __ATOMIC_RELAXED,
				      __ATOMIC_RELAXED));
#endif
    return (p64_ringbuf_result_t){ .index = tail,
				   .actual = actual,
				   .mask = mask };
}

#define LOWER(x) (uint32_t)(x)
#define TOLOWER(x) (uint32_t)(x)
#define UPPER(x) (uint32_t)((x) >> 32)
#define TOUPPER(x) ((uint64_t)(x) << 32)

static inline void
release_slots(struct idxpair *loc,
	      ringidx_t idx,
	      uint32_t n,
	      bool loads_only,
	      uint32_t flags)
{
    if (flags & FLAG_BLK)
    {
	//Wait for our turn to signal consumers (producers)
	wait_until_equal_w_bkoff(&loc->cur, idx, 50, __ATOMIC_RELAXED);
    }
    if (!(flags & FLAG_NONBLK))
    {
	//Release elements to consumers (producers)
	//Also enable other producers (consumers) to proceed
	if (loads_only)
	{
	    smp_fence(LoadStore);//Order loads only
	    atomic_store_n(&loc->cur, idx + n, __ATOMIC_RELAXED);
	}
	else
	{
	    atomic_store_n(&loc->cur, idx + n, __ATOMIC_RELEASE);
	}
	return;
    }
    //Else non-blocking (FLAG_NONBLK)
    uint64_t old, neu;
    for (;;)
    {
	//Attempt in-order release
	//Pending mask is clear before and after update
	old = TOLOWER(idx);
	neu = TOLOWER(idx + n);
	if (UNLIKELY(atomic_compare_exchange_n((uint64_t *)loc,
						 &old,
						 neu,
						 __ATOMIC_RELEASE,
						 __ATOMIC_RELAXED)))
	{
	    return;
	}
	//Else failure, 'old' updated with current value
	uint32_t delta = LOWER(neu) - LOWER(old);
	//Check if slots to release fit within pending window
	if (LIKELY(delta <= PENDMAX))
	{
	    break;
	}
	//Else end index outside of pending mask
	//This includes releases larger than the pending window which must be done in-order
	//Cannot perform update now, must wait and try again
	doze();
    }
    do
    {
	assert(n < PENDMAX);
	uint32_t offset = idx - LOWER(old);//Offset into pending mask
	assert(n + offset <= PENDMAX);
	uint32_t ourpend = ((1U << n) - 1) << offset;//Mask of slots to release
	assert((UPPER(old) & ourpend) == 0);//No overlap with already pending slots
	//newpend is wider so that it can be shifted PENDMAX bits if necessary
	uint64_t newpend = UPPER(old) | ourpend;//Update pending mask
	//Find number of in-order slots (count trailing ones of newpend)
	assert(~newpend != 0);//__builtin_ctz argument must not be 0
	uint32_t inorder = __builtin_ctzl(~newpend); //0..PENDMAX
	assert(inorder <= PENDMAX);
	neu = TOLOWER(old + inorder) | TOUPPER(newpend >> inorder);
	assert((UPPER(neu) & 1) == 0);//Lsb can't be pending since it is in-order
    }
    while (!atomic_compare_exchange_n((uint64_t *)loc,
					&old,//Updated on failure
					neu,
					__ATOMIC_RELEASE,
					__ATOMIC_RELAXED));
}

inline p64_ringbuf_result_t
p64_ringbuf_acquire_(p64_ringbuf_t *rb,
		     uint32_t num,
		     bool enqueue)
{
    uint32_t prod_flags = PROD_FLAGS(rb);
    uint32_t cons_flags = CONS_FLAGS(rb);
    rb = RB(rb);
    p64_ringbuf_result_t r;
    if (enqueue)
    {
	if (!(prod_flags & (FLAG_BLK | FLAG_NONBLK)))
	{
	    //MT-unsafe single producer code
	    //Consumer metadata is swapped: cons.tail<->cons.head
	    r = acquire_slots(&rb->prod.head.cur,
			      &rb->cons.head/*tail*/.cur,
			      rb->prod_mask, num, rb->prod.capacity);
	}
	else
	{
	    //MT-safe multi producer code
	    r = acquire_slots_mtsafe(&rb->prod, num);
	}
    }
    else //dequeue
    {
	if (cons_flags & FLAG_LOCKFREE)
	{
	    uint32_t mask = rb->cons_mask;

	    //Use prod.head instead of cons.head (which is not used at all)
	    int actual;
	    //Speculative acquisition of slots
	    ringidx_t head = atomic_load_n(&rb->prod.head.cur,
					     __ATOMIC_RELAXED);
	    //Consumer metadata is swapped: cons.tail<->cons.head
	    ringidx_t tail = atomic_load_n(&rb->cons.head/*tail*/.cur,
					     __ATOMIC_ACQUIRE);
	    actual = MIN((int)num, (int)(tail - head));
	    if (UNLIKELY(actual <= 0))
	    {
		return (p64_ringbuf_result_t){ .index = 0,
					       .actual = 0,
					       .mask = 0,
					       .ring = NULL };
	    }
	    return (p64_ringbuf_result_t){ .index = head,
					   .actual = actual,
					   .mask = mask,
					   .ring = rb->ring };
	}

	if (!(cons_flags & (FLAG_BLK | FLAG_NONBLK)))
	{
	    //MT-unsafe single consumer code
	    //Consumer metadata is swapped: cons.tail<->cons.head
	    r = acquire_slots(&rb->cons.head/*tail*/.cur,
			      &rb->prod.head.cur,
			      rb->cons_mask, num, 0);
	}
	else
	{
	    //MT-safe multi consumer code
	    r = acquire_slots_mtsafe(&rb->cons, num);
	}
    }
    r.ring = rb->ring;
    return r;
}

inline bool
p64_ringbuf_release_(p64_ringbuf_t *rb,
		     p64_ringbuf_result_t r,
		     bool enqueue)
{
    uint32_t prod_flags = PROD_FLAGS(rb);
    uint32_t cons_flags = CONS_FLAGS(rb);
    rb = RB(rb);

    if (enqueue)
    {
	//Consumer metadata is swapped: cons.tail<->cons.head
	release_slots(&rb->cons.head/*tail*/, r.index, r.actual,
		      /*loads_only=*/false, prod_flags);
	return true;//Success
    }
    else //dequeue
    {
	if (cons_flags & FLAG_LOCKFREE)
	{
	    bool success = atomic_compare_exchange_n(&rb->prod.head.cur,
						       &r.index,
						       r.index + r.actual,
						       __ATOMIC_RELEASE,
						       __ATOMIC_RELAXED);
	    return success;
	}
	release_slots(&rb->prod.head, r.index, r.actual,
		      /*loads_only=*/true, cons_flags);
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
    uint32_t prod_flags = PROD_FLAGS(rb);
    rb = RB(rb);
    //Step 1: acquire slots
    p64_ringbuf_result_t r;

    if (!(prod_flags & (FLAG_BLK | FLAG_NONBLK)))//SPENQ
    {
	//MT-unsafe single producer code
	//Consumer metadata is swapped: cons.tail<->cons.head
	r = acquire_slots(&rb->prod.head.cur,
			  &rb->cons.head/*tail*/.cur,
			  rb->prod_mask, num, rb->prod.capacity);
    }
    else//MPENQ or NBENQ
    {
	//MT-safe multi producer code
	r = acquire_slots_mtsafe(&rb->prod, num);
    }
    if (UNLIKELY(r.actual == 0))
    {
	return 0;
    }

    //Step 2: write slots
    write_slots(rb->ring, ev, r);

    //Step 3: release slots to consumer
    //Consumer metadata is swapped: cons.tail<->cons.head
    release_slots(&rb->cons.head/*tail*/, r.index, r.actual,
		  /*loads_only=*/false, prod_flags);

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
    uint32_t cons_flags = CONS_FLAGS(rb);
    rb = RB(rb);

    if (cons_flags & FLAG_LOCKFREE)
    {
	//Use prod.head instead of cons.head (which is not used at all)
	int actual;
	//Step 1: speculative acquisition of slots
	//Consumer metadata is swapped: cons.tail<->cons.head
	ringidx_t tail = atomic_load_n(&rb->cons.head/*tail*/.cur,
					 __ATOMIC_ACQUIRE);
	ringidx_t mask = rb->cons_mask;
#ifdef __ARM_FEATURE_ATOMICS
	ringidx_t head = atomic_icas_n(&rb->prod.head.cur, __ATOMIC_RELAXED);
#else
	PREFETCH_FOR_WRITE(&rb->prod.head);
	ringidx_t head = atomic_load_n(&rb->prod.head.cur, __ATOMIC_RELAXED);
#endif
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
	while (!atomic_compare_exchange_n(&rb->prod.head.cur,
					    &head,//Updated on failure
					    head + actual,
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
			  rb->cons_mask, num, 0);
    }
    else//MCDEQ or NBDEQ
    {
	//MT-safe multi consumer code
	r = acquire_slots_mtsafe(&rb->cons, num);
    }
    if (UNLIKELY(r.actual == 0))
    {
	return 0;
    }

    //Step 2: read slots
    read_slots(rb->ring, ev, r);

    //Step 3: release slots to producer
    release_slots(&rb->prod.head, r.index, r.actual,
		  /*loads_only=*/true, cons_flags);

    *index = r.index;
    return r.actual;
}
