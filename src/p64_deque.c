//Copyright (c) 2025, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <assert.h>
#include <stddef.h>

#include "p64_deque.h"

#include "atomic.h"
#include "common.h"
#include "err_hnd.h"

#define MARK_UNSTABLE (uintptr_t)1  //Pointer is marked as unstable
#define HAS_MARK(ptr) (((uintptr_t)(ptr) & MARK_UNSTABLE) != 0)
#define REM_MARK(ptr) (void *)((uintptr_t)(ptr) & ~MARK_UNSTABLE)
#define SET_MARK(ptr) (void *)((uintptr_t)(ptr) | MARK_UNSTABLE)

#define L 0
#define R 1

//Needed for custom double-word CAS
union p64_deque
{
    p64_deque_t deq;
    ptrpair_t pp;
};

void
p64_deque_init(p64_deque_t *deq)
{
    deq->end[L] = NULL;
    deq->end[R] = NULL;
}

static inline bool
is_stable(p64_deque_t deq)
{
    return !(HAS_MARK(deq.end[L]) | HAS_MARK(deq.end[R]));
}

static void
deque_stabilize(p64_deque_t *deq, union p64_deque mem)
{
    //'lr' indicates which end to stabilise
    uint32_t lr = HAS_MARK(mem.deq.end[R]) ? R : L;
    //The opposite end should not be unstable
    assert(!HAS_MARK(mem.deq.end[!lr]));
    //Get a clean pointer to the successor end element - 'succ'
    p64_deque_elem_t *succ = REM_MARK(mem.deq.end[lr]);
    //Read the pointer to the predecessor end element - 'pred'
    p64_deque_elem_t *pred = atomic_load_ptr(&succ->elem[!lr], __ATOMIC_ACQUIRE);
    //Read pred's successor pointer - 'predsucc'
    p64_deque_elem_t *predsucc = atomic_load_ptr(&pred->elem[lr], __ATOMIC_RELAXED);
    //Check of the successor pointer has already been updated
    if (predsucc != succ)
    {
	//No, wrong pointer, try to swing it
	if (!atomic_compare_exchange_ptr(&pred->elem[lr],
					 &predsucc,
					 succ,
					 __ATOMIC_RELAXED,
					 __ATOMIC_RELAXED))
	{
	    //CAS failed, pred's successor pointer already updated
	    //XXX shouldn't we also remove the unstable mark?
//	    return;
	}
	//Else update succeeded, the lr end has now been stabilized
    }
    //The lr end is now stable, remove unstable mark
    union p64_deque swp;
    swp.deq.end[lr] = REM_MARK(mem.deq.end[lr]);
    swp.deq.end[!lr] = mem.deq.end[!lr];
    assert(is_stable(swp.deq));
    //Attempt to write back unmarked pointers
    //We don't care if CAS fails, it means some other operation has commenced
    (void)atomic_compare_exchange_n((ptrpair_t *)deq,
				    &mem.pp,
				    swp.pp,
				    __ATOMIC_RELAXED,
				    __ATOMIC_RELAXED);
}

static inline p64_deque_t
atomic_load_deque(p64_deque_t *deq, int mo)
{
    union p64_deque mem;
#ifdef __ARM_FEATURE_ATOMICS
    mem.pp = atomic_icas_n((ptrpair_t *)deq, mo);
#else
    //Generic implementation (ignnoring Alpha) using address dependencies to control order
    //On x86-64, every scalar load is a load-acquire
    p64_deque_elem_t *a, *b, *c;
    do
    {
        a = atomic_load_ptr(         &deq->end[0],     mo);
        b = atomic_load_ptr(addr_dep(&deq->end[1], a), mo);
        c = atomic_load_ptr(addr_dep(&deq->end[0], b), __ATOMIC_RELAXED);
    }
    while (a != c);
    //There is an ABA problem in that the NULL value may be reused
    if (a == NULL)
    {
	//If end[0] equalled NULL, then end[1] must also have been NULL
	b = NULL;
    }
    assert((a == NULL && b == NULL) || (a != NULL && b != NULL));
    mem.deq.end[0] = a;
    mem.deq.end[1] = b;
#endif
    return mem.deq;
}

static void
deque_enqueue(p64_deque_t *deq, p64_deque_elem_t *elem, uint32_t lr)
{
    if (UNLIKELY(REM_MARK(elem) == NULL))
    {
	report_error("deque", "enqueue NULL element", elem);
	return;
    }
    if (UNLIKELY(HAS_MARK(elem)))
    {
	report_error("deque", "element has low bit set", elem);
	//Clear lsb and continue with insertion
	elem = REM_MARK(elem);
    }
    elem->elem[L] = NULL;
    elem->elem[R] = NULL;
    union p64_deque mem, swp;
    mem.deq = atomic_load_deque(deq, __ATOMIC_ACQUIRE);
    for (;;)
    {
	if (mem.deq.end[L] == NULL)
	{
	    assert(mem.deq.end[R] == NULL);
	    //Dequeue is empty, swap in 'elem' as only element, status = stable
	    swp.deq.end[L] = elem;
	    swp.deq.end[R] = elem;
	    if (atomic_compare_exchange_n((ptrpair_t *)deq, &mem.pp, swp.pp, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
	    {
		return;
	    }
	    //Else CAS failed, 'mem' updated
	}
	else if (is_stable(mem.deq))
	{
	    //Dequeue is not empty but is stable
	    //'elem' is new lrmost element, it points to the previous lrmost element
	    elem->elem[!lr] = deq->end[lr];
	    swp.deq.end[lr] = SET_MARK(elem);
	    swp.deq.end[!lr] = mem.deq.end[!lr];
	    if (atomic_compare_exchange_n((ptrpair_t *)deq,
					  &mem.pp,
					  swp.pp,
					  __ATOMIC_ACQ_REL,
					  __ATOMIC_ACQUIRE))
	    {
		//Swapped in new element at left/right end but deque is unstable
		deque_stabilize(deq, swp);
		return;
	    }
	    //Else CAS failed, 'mem' updated
	}
	else
	{
	    //Dequeue is not empty and not stable
	    deque_stabilize(deq, mem);
	    //Now retry our operation with a fresh value of '*deq'
	    mem.deq = atomic_load_deque(deq, __ATOMIC_ACQUIRE);
	}
    }
}

void
p64_deque_enqueue_l(p64_deque_t *deq, p64_deque_elem_t *elem)
{
    deque_enqueue(deq, elem, L);
}

void
p64_deque_enqueue_r(p64_deque_t *deq, p64_deque_elem_t *elem)
{
    deque_enqueue(deq, elem, R);
}

static p64_deque_elem_t *
deque_dequeue(p64_deque_t *deq, uint32_t lr)
{
    union p64_deque mem, swp;
    mem.deq = atomic_load_deque(deq, __ATOMIC_ACQUIRE);
    for (;;)
    {
	if (mem.deq.end[L] == NULL)
	{
	    assert(mem.deq.end[R] == NULL);
	    return NULL;
	}
	else if (mem.deq.end[L] == mem.deq.end[R])
	{
	    assert(is_stable(mem.deq));
	    //Deque only contains one element, try to remove it
	    swp.deq.end[L] = NULL;
	    swp.deq.end[R] = NULL;
	    if (atomic_compare_exchange_n((ptrpair_t *)deq, &mem.pp, swp.pp, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE))
	    {
		return mem.deq.end[L];//Same as mem.deq.end[R]
	    }
	    //Else CAS failed, 'mem' updated
	    continue;
	}
	else if (is_stable(mem.deq))
	{
	    //Deque contains more than one element
	    swp.deq.end[lr] = atomic_load_ptr(&mem.deq.end[lr]->elem[!lr], __ATOMIC_RELAXED);
	    swp.deq.end[!lr] = mem.deq.end[!lr];
	    if (atomic_compare_exchange_n((ptrpair_t *)deq, &mem.pp, swp.pp, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE))
	    {
		return mem.deq.end[lr];
	    }
	    //Else CAS failed, 'mem' updated
	    continue;
	}
	else
	{
	    //Dequeue is not stable
	    deque_stabilize(deq, mem);
	    //Now retry our operation with a fresh value of '*deq'
	    mem.deq = atomic_load_deque(deq, __ATOMIC_ACQUIRE);
	}
    }
}

p64_deque_elem_t *
p64_deque_dequeue_l(p64_deque_t *deq)
{
    return deque_dequeue(deq, L);
}

p64_deque_elem_t *
p64_deque_dequeue_r(p64_deque_t *deq)
{
    return deque_dequeue(deq, R);
}
