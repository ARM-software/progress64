//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "p64_msqueue.h"
#include "p64_hazardptr.h"
#include "p64_spinlock.h"
#include "build_config.h"
#include "arch.h"
#include "atomic.h"
#include "err_hnd.h"

#define TAG_INC 4

//Use last byte of msqueue data structure (tag) for spin lock
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define MSQ_TO_LOCK(qhead) &((p64_spinlock_t *)((qhead) + 1))[-1]
#endif

#define MSQ_NULL(msq) ((void *)qhead)

#define NOTINQUEUE (~0UL)

#ifndef NDEBUG
static unsigned
msqueue_assert(p64_ptr_tag_t *qhead, p64_ptr_tag_t *qtail)
{
    unsigned num = 0;
    p64_msqueue_elem_t *node;
    assert(qhead->ptr != MSQ_NULL(qhead));
    assert(qtail->ptr != MSQ_NULL(qhead));
    node = qhead->ptr;
    assert(node->next.tag != NOTINQUEUE);
    while (node->next.ptr != MSQ_NULL(qhead))
    {
	num++;
	node = node->next.ptr;
	assert(node->next.tag != NOTINQUEUE);
    }
    assert(qtail->ptr == node);
    return num;
}
#else
static unsigned
msqueue_assert(p64_ptr_tag_t *qhead, p64_ptr_tag_t *qtail)
{
    (void)qhead;
    (void)qtail;
    return 0;
}
#endif

void
p64_msqueue_init(p64_ptr_tag_t *qhead, p64_ptr_tag_t *qtail,
		 uint32_t aba_workaround, p64_msqueue_elem_t *dummy)
{
    if (aba_workaround > P64_ABA_SMR)
    {
	report_error("msqueue", "invalid ABA workaround", aba_workaround);
	return;
    }
    dummy->next.ptr = MSQ_NULL(qhead);
    dummy->next.tag = aba_workaround;
    qhead->ptr = dummy;
    qhead->tag = aba_workaround;//2 lsb of tag is aba_workaround
    if (aba_workaround == P64_ABA_LOCK)
    {
	p64_spinlock_t *lock = MSQ_TO_LOCK(qhead);
	p64_spinlock_init(lock);
	assert(qhead->tag == aba_workaround);
    }
    qtail->ptr = dummy;
    qtail->tag = aba_workaround;//2 lsb of tag is aba_workaround
    if (aba_workaround == P64_ABA_LOCK)
    {
	p64_spinlock_t *lock = MSQ_TO_LOCK(qtail);
	p64_spinlock_init(lock);
	assert(qtail->tag == aba_workaround);
    }
    assert(msqueue_assert(qhead, qtail) == 0);
}

p64_msqueue_elem_t *
p64_msqueue_fini(p64_ptr_tag_t *qhead, p64_ptr_tag_t *qtail)
{
    (void)qtail;
    if (qhead->ptr->next.ptr != MSQ_NULL(qhead))
    {
	report_error("msqueue", "queue not empty", qhead);
	return NULL;
    }
    return qhead->ptr;
}

static void
report_data_size_too_large(uint32_t size)
{
    report_error("msqueue", "data size too large", size);
}

static void
enqueue_lock(p64_ptr_tag_t *qhead,
	     p64_ptr_tag_t *qtail,
	     p64_msqueue_elem_t *elem)
{
#ifdef NDEBUG
    p64_spinlock_t *lock = MSQ_TO_LOCK(qtail);
#else
    //Use same lock in enqueue and dequeue when assertions enabled
    p64_spinlock_t *lock = MSQ_TO_LOCK(qhead);
#endif
    assert(elem->next.tag == NOTINQUEUE);
    elem->next.tag = P64_ABA_LOCK;
    elem->next.ptr = MSQ_NULL(qhead);
    p64_spinlock_acquire(lock);
    unsigned num = msqueue_assert(qhead, qtail); (void)num;
    qtail->ptr->next.ptr = elem;
    qtail->ptr = elem;
    assert(msqueue_assert(qhead, qtail) == num + 1);
    p64_spinlock_release(lock);
}

static struct p64_ptr_tag
atomic_load_ptr_tag(struct p64_ptr_tag *loc, int mo)
{
    struct p64_ptr_tag pt;
    __atomic_load(loc, &pt, mo);
#if defined __aarch64__
    //Use address dependency to force program order of loads
    do
    {
	pt.tag = atomic_load_n(&loc->tag, mo);
	pt.ptr = atomic_load_ptr(addr_dep(&loc->ptr, pt.tag), __ATOMIC_RELAXED);
	//The tag is strictly increasing, if it hasn't changed, the pointer
	//also hasn't changed
    }
    while (atomic_load_n(addr_dep(&loc->tag, pt.ptr), __ATOMIC_RELAXED) != pt.tag);
#else
    (void)mo;
    //Use load-acquire to force program order of loads
    do
    {
	pt.tag = atomic_load_n(&loc->tag, __ATOMIC_ACQUIRE);
	pt.ptr = atomic_load_n(&loc->ptr, __ATOMIC_ACQUIRE);
	//The tag is strictly increasing, if it hasn't changed, the pointer
	//also hasn't changed
    }
    while (atomic_load_n(&loc->tag, __ATOMIC_RELAXED) != pt.tag);
#endif
    return pt;
}

static int
atomic_cas_ptr_tag(struct p64_ptr_tag *loc,
		   struct p64_ptr_tag old,
		   struct p64_ptr_tag neu,
		   int mo_success,
		   int mo_failure)
{
    union
    {
	ptrpair_t pp;
	struct p64_ptr_tag pt;
    } xxx;
    xxx.pt = neu;
    return atomic_compare_exchange_n((ptrpair_t *)loc,
				    (ptrpair_t *)&old,
				    xxx.pp,
				    mo_success,
				    mo_failure);
}

static void
enqueue_tag(p64_ptr_tag_t *qhead, p64_ptr_tag_t *qtail, p64_msqueue_elem_t *node)
{
    struct p64_ptr_tag tail, next;
    assert(node->next.tag == NOTINQUEUE);
    node->next.tag = P64_ABA_TAG;
    node->next.ptr = MSQ_NULL(qhead);
    for (;;)
    {
	tail = atomic_load_ptr_tag(qtail, __ATOMIC_ACQUIRE);
	next = atomic_load_ptr_tag(&tail.ptr->next, __ATOMIC_RELAXED);
	if (tail.tag != atomic_load_n(&qtail->tag, __ATOMIC_RELAXED))
	{
	    continue;
	}
	if (next.ptr != MSQ_NULL(qhead))
	{
	    //qtail does not point to last node in list
	    //Update qtail
	    atomic_cas_ptr_tag(qtail,
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
    //Update qtail to point to the inserted node
    atomic_cas_ptr_tag(qtail,
		       tail,
		       (struct p64_ptr_tag){ node, tail.tag + TAG_INC},
		       __ATOMIC_RELEASE,
		       __ATOMIC_RELAXED);
}

static void
enqueue_smr(p64_ptr_tag_t *qhead, p64_ptr_tag_t *qtail, p64_msqueue_elem_t *node)
{
    p64_hazardptr_t hp = P64_HAZARDPTR_NULL;
    p64_msqueue_elem_t *tail, *next;
    assert(node->next.tag == NOTINQUEUE);
    node->next.tag = P64_ABA_SMR;
    node->next.ptr = MSQ_NULL(qhead);
    for (;;)
    {
	tail = p64_hazptr_acquire(&qtail->ptr, &hp);
	next = atomic_load_ptr(&tail->next.ptr, __ATOMIC_ACQUIRE);
	//Don't know if this check is actually required when using hazard
	//pointers, p64_hazptr_acquire() is performing a similar check
	//internally
	if (tail != atomic_load_ptr(&qtail->ptr, __ATOMIC_RELAXED))
	{
	    continue;
	}
	if (next != MSQ_NULL(qhead))
	{
	    //There is a node after 'next'
	    //qtail does not point to last node
	    //Advance qtail
	    (void)atomic_compare_exchange_ptr(&qtail->ptr,
					&tail,
					next,
					__ATOMIC_RELAXED,
					__ATOMIC_RELAXED);
	    //Don't care if we fail, some other thread did it
	    continue;
	}
	//Now next == NULL, tail points to last node in list
	//Insert new node after last node
	if (atomic_compare_exchange_ptr(&tail->next.ptr,
					&next,//NULL
					node,
					__ATOMIC_RELEASE,
					__ATOMIC_RELAXED))
	{
	    //qtail does not point to last node in list
	    break;
	}
    }
    //Update qtail to point to new last node
    (void)atomic_compare_exchange_ptr(&qtail->ptr,
				      &tail,
				      node,
				      __ATOMIC_RELEASE,
				      __ATOMIC_RELAXED);
    p64_hazptr_release(&hp);
}

//Enqueue at tail
void
p64_msqueue_enqueue(p64_ptr_tag_t *qhead,
		    p64_ptr_tag_t *qtail,
		    p64_msqueue_elem_t *elem,
		    const void *data,
		    uint32_t size)
{
    if (UNLIKELY(size > elem->max_size))
    {
	report_data_size_too_large(size);
	return;
    }
    memcpy(elem->data, data, elem->cur_size = size);
    uint32_t aba = qtail->tag % TAG_INC;
    switch (aba)
    {
	case P64_ABA_LOCK :
	    enqueue_lock(qhead, qtail, elem);
	    break;
	case P64_ABA_TAG :
	    enqueue_tag(qhead, qtail, elem);
	    break;
	case P64_ABA_SMR :
	    enqueue_smr(qhead, qtail, elem);
	    break;
	default :
	    UNREACHABLE();
    }
}

static p64_msqueue_elem_t *
dequeue_lock(p64_ptr_tag_t *qhead,
	     p64_ptr_tag_t *qtail,
	     void *data,
	     uint32_t *size)
{
    p64_msqueue_elem_t *elem = NULL;
    p64_msqueue_elem_t *head;
    p64_spinlock_t *lock = MSQ_TO_LOCK(qhead);
    p64_spinlock_acquire(lock);
    unsigned num = msqueue_assert(qhead, qtail); (void)num;
    head = qhead->ptr;
    if (LIKELY(head->next.ptr != MSQ_NULL(qhead)))
    {
	if (head->next.ptr->cur_size > *size)
	{
	    report_data_size_too_large(head->next.ptr->cur_size);
	    goto leave;
	}
	memcpy(data, head->next.ptr->data, *size = head->next.ptr->cur_size);
	qhead->ptr = head->next.ptr;
	elem = head;
	assert(msqueue_assert(qhead, qtail) == num - 1);
    }
    //Else only dummy node in msqueue
leave:
    p64_spinlock_release(lock);
    if (elem != NULL)
    {
	assert(elem->next.tag != NOTINQUEUE);
	elem->next.tag = NOTINQUEUE;
    }
    return elem;
}

static p64_msqueue_elem_t *
dequeue_tag(p64_ptr_tag_t *qhead,
	    p64_ptr_tag_t *qtail,
	    void *data,
	    uint32_t *size)
{
    struct p64_ptr_tag head, tail;
    p64_msqueue_elem_t *next;
    for (;;)
    {
	head = atomic_load_ptr_tag(qhead, __ATOMIC_ACQUIRE);
	tail = atomic_load_ptr_tag(qtail, __ATOMIC_RELAXED);
	next = atomic_load_ptr(&head.ptr->next.ptr, __ATOMIC_ACQUIRE);
	if (head.tag != atomic_load_n(&qhead->tag, __ATOMIC_RELAXED))
	{
	    continue;
	}
	if (head.ptr == tail.ptr)
	{
	    if (next == MSQ_NULL(qhead))
	    {
		return NULL;
	    }
	    //Tail has fallen behind, attempt to advance it
	    atomic_cas_ptr_tag(qtail,
			       tail,
			       (struct p64_ptr_tag){ next, tail.tag + TAG_INC},
			       __ATOMIC_RELAXED,
			       __ATOMIC_RELAXED);
	    continue;
	}
	//Else head.ptr != tail.ptr
	//Read data before CAS or we will race with another dequeue
	if (next->cur_size > *size)
	{
	    report_data_size_too_large(next->cur_size);
	    return NULL;
	}
	memcpy(data, next->data, *size = next->cur_size);
	if (atomic_cas_ptr_tag(qhead,
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
    return head.ptr;
}

static p64_msqueue_elem_t *
dequeue_smr(p64_ptr_tag_t *qhead,
	    p64_ptr_tag_t *qtail,
	    void *data,
	    uint32_t *size)
{
    p64_hazardptr_t hp0 = P64_HAZARDPTR_NULL;
    p64_hazardptr_t hp1 = P64_HAZARDPTR_NULL;
    p64_msqueue_elem_t *head, *tail, *next;
    for (;;)
    {
	head = p64_hazptr_acquire(&qhead->ptr, &hp0);
	tail = atomic_load_ptr(&qtail->ptr, __ATOMIC_RELAXED);
	next = p64_hazptr_acquire(&head->next.ptr, &hp1);
	//Don't know if this check is actually required when using hazard
	//pointers, p64_hazptr_acquire() is performing a similar check
	//internally
	if (head != atomic_load_ptr(&qhead->ptr, __ATOMIC_RELAXED))
	{
	    continue;
	}
	if (next == MSQ_NULL(qhead))
	{
	    p64_hazptr_release(&hp0);
	    p64_hazptr_release(&hp1);
	    return NULL;
	}
	//Else next != NULL
	if (head == tail)
	{
	    //Queue looks empty but head->next is a valid node
	    (void)atomic_compare_exchange_ptr(&qtail->ptr,
					      &tail,
					      next,
					      __ATOMIC_RELAXED,
					      __ATOMIC_RELAXED);
	    continue;
	}
	//Else head != tail and next != NULL
	//next is protected by a hazard pointer and thus will be valid until
	//we release the hazard pointer, i.e. also after the CAS below
	if (atomic_compare_exchange_ptr(&qhead->ptr,
					&head,
					next,
					__ATOMIC_RELEASE,
					__ATOMIC_RELAXED))
	{
	    //head is now ours
	    if (next->cur_size > *size)
	    {
		report_data_size_too_large(next->cur_size);
		goto leave;
	    }
	    memcpy(data, next->data, *size = next->cur_size);
	    break;
	}
    }
    assert(head->next.tag != NOTINQUEUE);
    head->next.tag = NOTINQUEUE;
leave:
    p64_hazptr_release(&hp0);
    p64_hazptr_release(&hp1);
    //Returned node must be retired and reclaimed before it is used again
    return head;
}

//Dequeue from head
p64_msqueue_elem_t *
p64_msqueue_dequeue(p64_ptr_tag_t *qhead,
		    p64_ptr_tag_t *qtail,
		    void *data,
		    uint32_t *size)
{
    uint32_t aba = qhead->tag % TAG_INC;
    switch (aba)
    {
	case P64_ABA_LOCK :
	    return dequeue_lock(qhead, qtail, data, size);
	case P64_ABA_TAG :
	    return dequeue_tag(qhead, qtail, data, size);
	case P64_ABA_SMR :
	    return dequeue_smr(qhead, qtail, data, size);
	default :
	    UNREACHABLE();
    }
}
