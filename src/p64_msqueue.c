//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "p64_msqueue.h"
#include "p64_hazardptr.h"
#include "p64_spinlock.h"
#include "build_config.h"
#include "arch.h"
#include "lockfree.h"
#ifndef __aarch64__
#define lockfree_compare_exchange_16_frail lockfree_compare_exchange_16
#endif
#ifdef __aarch64__
#include "ldxstx.h"
#endif

#define TAG_INC 4

//Use last byte of msqueue data structure (tag) for spin lock
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define MSQ_TO_LOCK(msq) &((p64_spinlock_t *)((msq) + 1))[-1]
#endif

#define MSQ_NULL(msq) ((void *)msq)

#define NOTINQUEUE (~0UL)

#ifndef NDEBUG
static unsigned
msqueue_assert(p64_msqueue_t *msq)
{
    unsigned num = 0;
    p64_msqueue_elem_t *node;
    assert(msq->head.ptr != MSQ_NULL(msq));
    assert(msq->tail.ptr != MSQ_NULL(msq));
    node = msq->head.ptr;
    assert(node->next.tag != NOTINQUEUE);
    while (node->next.ptr != MSQ_NULL(msq))
    {
	num++;
	node = node->next.ptr;
	assert(node->next.tag != NOTINQUEUE);
    }
    assert(msq->tail.ptr == node);
    return num;
}
#else
static unsigned
msqueue_assert(p64_msqueue_t *msq)
{
    (void)msq;
    return 0;
}
#endif

void
p64_msqueue_init(p64_msqueue_t *msq, uint32_t aba_workaround, p64_msqueue_elem_t *dummy)
{
    if (aba_workaround > P64_ABA_SMR)
    {
	fprintf(stderr, "msqueue: Invalid aba_workaround %u\n", aba_workaround);
	fflush(stderr);
	abort();
    }
    dummy->next.ptr = MSQ_NULL(msq);
    dummy->next.tag = aba_workaround;
    msq->head.ptr = dummy;
    msq->head.tag = aba_workaround;//2 lsb of tag is aba_workaround
    if (aba_workaround == P64_ABA_LOCK)
    {
	p64_spinlock_t *lock = MSQ_TO_LOCK(&msq->head);
	p64_spinlock_init(lock);
	assert(msq->head.tag == aba_workaround);
    }
    msq->tail.ptr = dummy;
    msq->tail.tag = aba_workaround;//2 lsb of tag is aba_workaround
    if (aba_workaround == P64_ABA_LOCK)
    {
	p64_spinlock_t *lock = MSQ_TO_LOCK(&msq->tail);
	p64_spinlock_init(lock);
	assert(msq->tail.tag == aba_workaround);
    }
    assert(msqueue_assert(msq) == 0);
}

p64_msqueue_elem_t *
p64_msqueue_fini(p64_msqueue_t *msq)
{
    if (msq->head.ptr->next.ptr != MSQ_NULL(msq))
    {
	fprintf(stderr, "msqueue %p not empty\n", msq);
	fflush(stderr);
	abort();
    }
    return msq->head.ptr;
}

static void
enqueue_lock(p64_msqueue_t *msq, p64_msqueue_elem_t *elem)
{
#ifdef NDEBUG
    p64_spinlock_t *lock = MSQ_TO_LOCK(&msq->tail);
#else
    //Use same lock in enqueue and dequeue when assertions enabled
    p64_spinlock_t *lock = MSQ_TO_LOCK(&msq->head);
#endif
    assert(elem->next.tag == NOTINQUEUE);
    elem->next.tag = P64_ABA_LOCK;
    elem->next.ptr = MSQ_NULL(msq);
    p64_spinlock_acquire(lock);
    unsigned num = msqueue_assert(msq); (void)num;
    msq->tail.ptr->next.ptr = elem;
    msq->tail.ptr = elem;
    assert(msqueue_assert(msq) == num + 1);
    p64_spinlock_release(lock);
}

static inline struct p64_ptr_tag
atomic_load_ptr_tag(const struct p64_ptr_tag *loc, int mo)
{
    (void)mo;
    struct p64_ptr_tag pt;
    do
    {
	pt.tag = __atomic_load_n(&loc->tag, __ATOMIC_ACQUIRE);
	pt.ptr = __atomic_load_n(&loc->ptr, __ATOMIC_ACQUIRE);
	//The tag is strictly increasing, if it hasn't changed, the pointer
	//also hasn't changed
    }
    while (__atomic_load_n(&loc->tag, __ATOMIC_RELAXED) != pt.tag);
    return pt;
}

static inline int
atomic_cas_ptr_tag(struct p64_ptr_tag *loc,
		   struct p64_ptr_tag old,
		   struct p64_ptr_tag neu,
		   int mo_success,
		   int mo_failure)
{
    union
    {
	__int128 i128;
	struct p64_ptr_tag pt;
    } xxx;
    xxx.pt = neu;
    return lockfree_compare_exchange_16((__int128 *)loc,
					(__int128 *)&old,
					xxx.i128,
					false,
					mo_success,
					mo_failure);
}

static void
enqueue_tag(p64_msqueue_t *msq, p64_msqueue_elem_t *node)
{
    struct p64_ptr_tag tail, next;
    assert(node->next.tag == NOTINQUEUE);
    node->next.tag = P64_ABA_TAG;
    node->next.ptr = MSQ_NULL(msq);
    for (;;)
    {
	tail = atomic_load_ptr_tag(&msq->tail, __ATOMIC_ACQUIRE);
	next = atomic_load_ptr_tag(&tail.ptr->next, __ATOMIC_RELAXED);
	if (tail.tag != __atomic_load_n(&msq->tail.tag, __ATOMIC_RELAXED))
	{
	    continue;
	}
	if (next.ptr != MSQ_NULL(msq))
	{
	    //msq->tail does not point to last node in list
	    //Update msq->tail
	    atomic_cas_ptr_tag(&msq->tail,
			       tail,
			       (struct p64_ptr_tag){ next.ptr,
						     tail.tag + TAG_INC},
			       __ATOMIC_RELAXED,
			       __ATOMIC_RELAXED);
	    continue;
	}
	//Now next.ptr == NULL, tail points to last node in list
	//Insert new node after last node
	if (atomic_cas_ptr_tag(&tail.ptr->next,
			       next,
			       (struct p64_ptr_tag){ node, next.tag + TAG_INC},
			       __ATOMIC_RELEASE,
			       __ATOMIC_RELAXED))
	{
	    break;
	}
    }
    //Update msq->tail to point to the inserted node
    atomic_cas_ptr_tag(&msq->tail,
		       tail,
		       (struct p64_ptr_tag){ node, tail.tag + TAG_INC},
		       __ATOMIC_RELEASE,
		       __ATOMIC_RELAXED);
}

static void
enqueue_smr(p64_msqueue_t *msq, p64_msqueue_elem_t *node)
{
    p64_hazardptr_t hp = P64_HAZARDPTR_NULL;
    p64_msqueue_elem_t *tail, *next;
    assert(node->next.tag == NOTINQUEUE);
    node->next.tag = P64_ABA_SMR;
    node->next.ptr = MSQ_NULL(msq);
    for (;;)
    {
	tail = p64_hazptr_acquire(&msq->tail.ptr, &hp);
	next = __atomic_load_n(&tail->next.ptr, __ATOMIC_ACQUIRE);
	//Don't know if this check is actually required when using hazard
	//pointers, p64_hazptr_acquire() is performing a similar check
	//internally
	if (tail != __atomic_load_n(&msq->tail.ptr, __ATOMIC_RELAXED))
	{
	    continue;
	}
	if (next != MSQ_NULL(msq))
	{
	    //There is a node after 'next'
	    //msq->tail does not point to last node
	    //Advance msq->tail
	    __atomic_compare_exchange_n(&msq->tail.ptr,
					&tail,
					next,
					false,
					__ATOMIC_RELAXED,
					__ATOMIC_RELAXED);
	    //Don't care if we fail, some other thread did it
	    continue;
	}
	//Now next == NULL, tail points to last node in list
	//Insert new node after last node
	if (__atomic_compare_exchange_n(&tail->next.ptr,
					&next,//NULL
					node,
					false,
					__ATOMIC_RELEASE,
					__ATOMIC_RELAXED))
	{
	    //msq->tail does not point to last node in list
	    break;
	}
    }
    //Update msq->tail to point to new last node
    __atomic_compare_exchange_n(&msq->tail.ptr,
				&tail,
				node,
				false,
				__ATOMIC_RELEASE,
				__ATOMIC_RELAXED);
    p64_hazptr_release(&hp);
}

//Enqueue at tail
void
p64_msqueue_enqueue(p64_msqueue_t *msq, p64_msqueue_elem_t *elem)
{
    uint32_t aba = msq->tail.tag % TAG_INC;
    switch (aba)
    {
	case P64_ABA_LOCK :
	    enqueue_lock(msq, elem);
	    break;
	case P64_ABA_TAG :
	    enqueue_tag(msq, elem);
	    break;
	case P64_ABA_SMR :
	    enqueue_smr(msq, elem);
	    break;
	default :
	    UNREACHABLE();
    }
}

static p64_msqueue_elem_t *
dequeue_lock(p64_msqueue_t *msq)
{
    p64_msqueue_elem_t *elem = NULL;
    p64_msqueue_elem_t *head;
    p64_spinlock_t *lock = MSQ_TO_LOCK(&msq->head);
    p64_spinlock_acquire(lock);
    unsigned num = msqueue_assert(msq); (void)num;
    head = msq->head.ptr;
    if (LIKELY(head->next.ptr != MSQ_NULL(msq)))
    {
	head->user_data = head->next.ptr->user_data;
	msq->head.ptr = head->next.ptr;
	elem = head;
	assert(msqueue_assert(msq) == num - 1);
    }
    //Else only dummy node in msqueue
    p64_spinlock_release(lock);
    if (elem != NULL)
    {
	assert(elem->next.tag != NOTINQUEUE);
	elem->next.tag = NOTINQUEUE;
    }
    return elem;
}

static p64_msqueue_elem_t *
dequeue_tag(p64_msqueue_t *msq)
{
    struct p64_ptr_tag head, tail;
    p64_msqueue_elem_t *next;
    void *data;
    for (;;)
    {
	head = atomic_load_ptr_tag(&msq->head, __ATOMIC_ACQUIRE);
	tail = atomic_load_ptr_tag(&msq->tail, __ATOMIC_RELAXED);
	next = __atomic_load_n(&head.ptr->next.ptr, __ATOMIC_ACQUIRE);
	if (head.tag != __atomic_load_n(&msq->head.tag, __ATOMIC_RELAXED))
	{
	    continue;
	}
	if (head.ptr == tail.ptr)
	{
	    if (next == MSQ_NULL(msq))
	    {
		return NULL;
	    }
	    //Tail has fallen behind, attempt to advance it
	    atomic_cas_ptr_tag(&msq->tail,
			       tail,
			       (struct p64_ptr_tag){ next, tail.tag + TAG_INC},
			       __ATOMIC_RELAXED,
			       __ATOMIC_RELAXED);
	    continue;
	}
	//Else head.ptr != tail.ptr
	//Read data before CAS or we will race with another dequeue
	data = next->user_data;
	if (atomic_cas_ptr_tag(&msq->head,
			       head,
			       (struct p64_ptr_tag){ next, head.tag + TAG_INC},
			       __ATOMIC_RELAXED,
			       __ATOMIC_RELAXED))
	{
	    break;
	}
    }
    assert(head.ptr->next.tag != NOTINQUEUE);
    head.ptr->next.tag = NOTINQUEUE;
    head.ptr->user_data = data;
    return head.ptr;
}

static p64_msqueue_elem_t *
dequeue_smr(p64_msqueue_t *msq)
{
    p64_hazardptr_t hp0 = P64_HAZARDPTR_NULL;
    p64_hazardptr_t hp1 = P64_HAZARDPTR_NULL;
    p64_msqueue_elem_t *head, *tail, *next;
    void *data;
    for (;;)
    {
	head = p64_hazptr_acquire(&msq->head.ptr, &hp0);
	tail = __atomic_load_n(&msq->tail.ptr, __ATOMIC_RELAXED);
	next = p64_hazptr_acquire(&head->next.ptr, &hp1);
	//Don't know if this check is actually required when using hazard
	//pointers, p64_hazptr_acquire() is performing a similar check
	//internally
	if (head != __atomic_load_n(&msq->head.ptr, __ATOMIC_RELAXED))
	{
	    continue;
	}
	if (next == MSQ_NULL(msq))
	{
	    p64_hazptr_release(&hp0);
	    p64_hazptr_release(&hp1);
	    return NULL;
	}
	//Else next != NULL
	if (head == tail)
	{
	    //Queue looks empty but head->next is a valid node
	    __atomic_compare_exchange_n(&msq->tail.ptr,
					&tail,
					next,
					false,
					__ATOMIC_RELAXED,
					__ATOMIC_RELAXED);
	    continue;
	}
	//Else head != tail and next != NULL
	//Read data before CAS
	//next is protected by a hazard pointer so we could read data later
	data = next->user_data;
	if (__atomic_compare_exchange_n(&msq->head.ptr,
					&head,
					next,
					false,
					__ATOMIC_RELEASE,
					__ATOMIC_RELAXED))
	{
	    break;
	}
    }
    assert(head->next.tag != NOTINQUEUE);
    head->next.tag = NOTINQUEUE;
    head->user_data = data;
    p64_hazptr_release(&hp0);
    p64_hazptr_release(&hp1);
    //Returned node must be retired and reclaimed before it is used again
    return head;
}

//Dequeue from head
p64_msqueue_elem_t *
p64_msqueue_dequeue(p64_msqueue_t *msq)
{
    uint32_t aba = msq->head.tag % TAG_INC;
    switch (aba)
    {
	case P64_ABA_LOCK :
	    return dequeue_lock(msq);
	case P64_ABA_TAG :
	    return dequeue_tag(msq);
	case P64_ABA_SMR :
	    return dequeue_smr(msq);
	default :
	    UNREACHABLE();
    }
}
