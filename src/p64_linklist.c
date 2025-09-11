//Copyright (c) 2025, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include "p64_linklist.h"

#include "atomic.h"
#include "err_hnd.h"

#define MARK_REMOVE (uintptr_t)1
#define HAS_MARK(ptr) (((uintptr_t)(ptr) & MARK_REMOVE) != 0)
#define REM_MARK(ptr) (void *)((uintptr_t)(ptr) & ~MARK_REMOVE)

void
p64_linklist_init(p64_linklist_t *list)
{
    list->next = NULL;
}

p64_linklist_t *
p64_linklist_next(p64_linklist_t *curr)
{
    if (curr == NULL)
    {
	report_error("linklist", "next NULL element", curr);
	return NULL;
    }
    //Current element becomes predecessor
    p64_linklist_t *pred = curr;
    //'curr' is element after pred
    curr = atomic_load_ptr(&pred->next, __ATOMIC_ACQUIRE);
    while (REM_MARK(curr) != NULL)
    {
	//Not end of list
	//We don't care whether 'pred'ecessor is marked for removal so remove any mark
	curr = REM_MARK(curr);
	//Read next pointer which also returns status of 'curr'
	p64_linklist_t *next = atomic_load_ptr(&curr->next, __ATOMIC_ACQUIRE);
	if (HAS_MARK(next))
	{
	    //'curr' marked for removal, try to remove it
	    next = REM_MARK(next);
	    if (atomic_compare_exchange_ptr(&pred->next,
					    &curr,//Updated on failure
					    next,
					    __ATOMIC_RELEASE,
					    __ATOMIC_RELAXED))
	    {
		//'curr' removed, 'pred' now points to 'next'
		//Keep 'pred', update 'curr'
		curr = next;
	    }
	    else //Failed to remove 'curr' from predecessor's next ptr
	    {
		//We continue anyway with the next element after 'pred->next'
		//'curr' already updated by CAS failure
	    }
	}
	else //'curr' is not marked for removal
	{
	    //Return 'curr'
	    return curr;
	}
    }
    //Reached end of list
    return NULL;
}

bool
p64_linklist_remove(p64_linklist_t *pred,
		    p64_linklist_t *elem)
{
    //Set REMOVE mark on our next pointer
    p64_linklist_t *next = atomic_fetch_or(&elem->next, MARK_REMOVE, __ATOMIC_RELAXED);
    do
    {
	//Now try to unlink us
	p64_linklist_t *this = elem;
	if (atomic_compare_exchange_ptr(&pred->next,
					&this,//Updated on failure
					REM_MARK(next),
					__ATOMIC_ACQUIRE,
					__ATOMIC_ACQUIRE))
	{
	    //Success, 'this' (== 'elem') element is removed
	    return true;
	}
	//Else CAS failed => unlinking failed
	//Check if predecessor element is marked for REMOVAL
	if (HAS_MARK(this))
	{
	    //Yes, it must first be removed but we can't do that here
	    return false;
	}
	//Else either some other thread helped remove our element
	//Or some other element was inserted after predecessor element (before us)
	pred = this;
    }
    while (pred != NULL);
    //'elem' not found in list, some other thread probably unlinked it
    return true;
}

bool
p64_linklist_insert(p64_linklist_t *pred,
		    p64_linklist_t *elem)
{
    if (UNLIKELY(HAS_MARK(elem)))
    {
	report_error("linklist", "element has low bit set", elem);
	//Clear lsb and continue with insertion
	elem = REM_MARK(elem);
    }
    if (UNLIKELY(elem == NULL))
    {
	report_error("linklist", "insert NULL element", elem);
	return true;
    }
    p64_linklist_t *next = atomic_load_ptr(&pred->next, __ATOMIC_ACQUIRE);
    for (;;)
    {
	if (HAS_MARK(next))
	{
	    //'pred' element is marked for removal
	    //Since we don't know 'pred's predecessor, we can't help remove 'pred'
	    //Instead try to insert after 'pred's successor
	    next = REM_MARK(next);
	    if (next == NULL)
	    {
		//No element after 'pred' to use as new predecessor, insertion fails
		return false;
	    }
	    //Update pred:next tuple for next attempt
	    pred = next;
	    next = atomic_load_ptr(&pred->next, __ATOMIC_ACQUIRE);
	}
	else
	{
	    //'pred' is not marked for removal, we can insert here
	    elem->next = next;
	    //Try to swing in new element
	    if (atomic_compare_exchange_ptr(&pred->next,
					    &next,//Updated on failure
					    elem,
					    __ATOMIC_RELEASE,
					    __ATOMIC_ACQUIRE))
	    {
		//Success inserting new element
		return true;
	    }
	    //Else pred->next changed
	    //Either 'pred' is being removed or some other element was inserted
	    //Restart with updated value of 'next', 'pred' remains the same
	}
    }
}
