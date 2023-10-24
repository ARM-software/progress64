//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <assert.h>
#include <stddef.h>
#include <stdlib.h>

#include "p64_stack.h"
#include "p64_hazardptr.h"
#include "p64_spinlock.h"
#include "build_config.h"
#include "arch.h"
#include "err_hnd.h"
#include "lockfree.h"

#define TAG_INCREMENT 4

//Use last byte of stack data structure (tag) for spin lock
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define STK_TO_LOCK(stk) &((p64_spinlock_t *)((stk) + 1))[-1]
#endif

void
p64_stack_init(p64_stack_t *stk, uint32_t aba_workaround)
{
#ifdef __aarch64__
    if (aba_workaround > P64_ABA_LLSC)
#else
    if (aba_workaround > P64_ABA_SMR)
#endif
    {
	report_error("stack", "invalid ABA workaround", aba_workaround);
	return;
    }
    stk->head = NULL;
    stk->tag = aba_workaround;//2 lsb of tag is aba_workaround
    if (aba_workaround == P64_ABA_LOCK)
    {
	p64_spinlock_t *lock = STK_TO_LOCK(stk);
	p64_spinlock_init(lock);
	assert(stk->tag == aba_workaround);
    }
}

static void
enqueue_lock(p64_stack_t *stk, p64_stack_elem_t *elem)
{
    p64_spinlock_t *lock = STK_TO_LOCK(stk);
    p64_spinlock_acquire(lock);
    elem->next = stk->head;
    stk->head = elem;
    p64_spinlock_release(lock);
}

static void
enqueue_tag(p64_stack_t *stk, p64_stack_elem_t *elem)
{
    union
    {
	ptrpair_t pp;
	p64_stack_t st;
    } old, neu;
    do
    {
	__atomic_load(&stk->head, &old.st.head, __ATOMIC_RELAXED);
	__atomic_load(&stk->tag, &old.st.tag, __ATOMIC_RELAXED);
	elem->next = old.st.head;
	neu.st.head = elem;
	neu.st.tag = old.st.tag + TAG_INCREMENT;
    }
    while (!lockfree_compare_exchange_pp_frail((ptrpair_t *)&stk->head,
					       &old.pp,
					       neu.pp,
					       /*weak=*/true,
					       __ATOMIC_RELEASE,
					       __ATOMIC_RELAXED));

}

//Call-back function invoked when object is guaranteed not to be referenced
//by any thread
static void
callback_smr(void *ptr)
{
    p64_stack_elem_t *elem = ptr;
    //Restore the stack "pointer" from the element
    p64_stack_t *stk = (p64_stack_t *)elem->next;
    //Perform actual enqueue
    p64_stack_elem_t *old;
    do
    {
	old = __atomic_load_n(&stk->head, __ATOMIC_RELAXED);
	elem->next = old;
    }
    while (UNLIKELY(!__atomic_compare_exchange_n(&stk->head,
						 &old,//Updated on failure
						 elem,
						 /*weak=*/true,
						 __ATOMIC_RELEASE,
						 __ATOMIC_RELAXED)));
}

static void
enqueue_smr(p64_stack_t *stk, p64_stack_elem_t *elem)
{
    //Save the stack "pointer" in the element to enqueue
    //We use the next pointer field for this as it is not yet used
    elem->next = (p64_stack_elem_t *)stk;
    //Retire element, defer actual enqueue to the reclaim callback when there
    //are no more references to the element
    //This deferred enqueue means LIFO order is not guaranteed
    while (!p64_hazptr_retire(elem, callback_smr))
    {
	//The internal retire buffer may be full, retry until there is space
	//for another element
	doze();
    }
    //Attempt immediate reclamation
    p64_hazptr_reclaim();
}

#ifdef __aarch64__
static void
enqueue_llsc(p64_stack_t *stk, p64_stack_elem_t *elem)
{
    p64_stack_elem_t *old, *head;
    do
    {
	head = __atomic_load_n(&stk->head, __ATOMIC_RELAXED);
	//Perform write outside of exclusives section
	elem->next = head;
	old = ldxptr(&stk->head, __ATOMIC_RELAXED);
    }
    while (UNLIKELY(old != head || stxptr(&stk->head, elem, __ATOMIC_RELEASE)));
}
#endif

void
p64_stack_enqueue(p64_stack_t *stk, p64_stack_elem_t *elem)
{
    switch (stk->tag % TAG_INCREMENT)
    {
	case P64_ABA_LOCK :
	    enqueue_lock(stk, elem);
	    break;
	case P64_ABA_TAG :
	    enqueue_tag(stk, elem);
	    break;
	case P64_ABA_SMR :
	    enqueue_smr(stk, elem);
	    break;
#ifdef __aarch64__
	case P64_ABA_LLSC :
	    enqueue_llsc(stk, elem);
	    break;
#endif
	default :
	    UNREACHABLE();
    }
}

static p64_stack_elem_t *
dequeue_lock(p64_stack_t *stk)
{
    p64_stack_elem_t *elem = NULL;
    p64_spinlock_t *lock = STK_TO_LOCK(stk);
    p64_spinlock_acquire(lock);
    if (LIKELY(stk->head != NULL))
    {
	elem = stk->head;
	stk->head = elem->next;
    }
    p64_spinlock_release(lock);
    return elem;
}

static p64_stack_elem_t *
dequeue_tag(p64_stack_t *stk)
{
    union
    {
	ptrpair_t pp;
	p64_stack_t st;
    } old, neu;
    do
    {
	__atomic_load(&stk->head, &old.st.head, __ATOMIC_ACQUIRE);
	__atomic_load(&stk->tag, &old.st.tag, __ATOMIC_RELAXED);
	if (old.st.head == NULL)
	{
	    return NULL;
	}
	//Dereferencing 'old' which might not be valid anymore
	//We assume the memory exists but the value read might be bogus
	neu.st.head = old.st.head->next;
	neu.st.tag = old.st.tag + TAG_INCREMENT;
    }
    while (!lockfree_compare_exchange_pp_frail((ptrpair_t *)&stk->head,
					       &old.pp,//Updated on failure
					       neu.pp,
					       /*weak=*/true,
					       __ATOMIC_RELAXED,
					       __ATOMIC_RELAXED));

    return old.st.head;
}

static p64_stack_elem_t *
dequeue_smr(p64_stack_t *stk)
{
    p64_hazardptr_t hp = P64_HAZARDPTR_NULL;
    p64_stack_elem_t *old, *neu;
    do
    {
	old = p64_hazptr_acquire(&stk->head, &hp);
	if (old == NULL)
	{
	    //If the stack is empty but the user expects to be able to dequeue
	    //an element, it could mean elements are stuck in the retire queue,
	    //waiting for reclamation to complete
	    p64_hazptr_reclaim();
	    old = p64_hazptr_acquire(&stk->head, &hp);
	    if (old == NULL)
	    {
		break;//Exit CAS-loop
	    }
	}
	//'old' is guaranteed to be valid due to hazard pointer protection
	//The 'old->next' field might be overwritten if other thread dequeued
	//and then attempted to enqueue the 'old' element. But since the element
	//is protected by our hazard pointer, the 'old' element will not
	//actually be enqueued (it would still be in the retire list) so the
	//CAS below would fail because 'stk->head' has changed
	neu = old->next;
    }
    while (UNLIKELY(!__atomic_compare_exchange_n(&stk->head,
						 &old,
						 neu,
						 /*weak=*/true,
						 __ATOMIC_RELAXED,
						 __ATOMIC_RELAXED)));
    p64_hazptr_release(&hp);
    return old;
}

#ifdef __aarch64__
static p64_stack_elem_t *
dequeue_llsc(p64_stack_t *stk)
{
    p64_stack_elem_t *old, *neu;
    do
    {
	old = ldxptr(&stk->head, __ATOMIC_ACQUIRE);
	if (old == NULL)
	{
	    return NULL;
	}
	//Load in exclusives section is not kosher on Arm
	neu = old->next;
    }
    while (UNLIKELY(stxptr(&stk->head, neu, __ATOMIC_RELAXED)));
    return old;
}
#endif

p64_stack_elem_t *
p64_stack_dequeue(p64_stack_t *stk)
{
    switch (stk->tag % TAG_INCREMENT)
    {
	case P64_ABA_LOCK :
	    return dequeue_lock(stk);
	case P64_ABA_TAG :
	    return dequeue_tag(stk);
	case P64_ABA_SMR :
	    return dequeue_smr(stk);
#ifdef __aarch64__
	case P64_ABA_LLSC :
	    return dequeue_llsc(stk);
#endif
	default :
	    UNREACHABLE();
    }
}
