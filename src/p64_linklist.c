//Copyright (c) 2025, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <assert.h>

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

void
p64_linklist_cursor_init(p64_linklist_cursor_t *curs, p64_linklist_t *list)
{
    if (list == NULL)
    {
	report_error("linklist", "NULL list", list);
    }
    curs->curr = list;
}

p64_linklist_status_t
p64_linklist_cursor_next(p64_linklist_cursor_t *curs)
{
    if (curs->curr == NULL)
    {
	report_error("linklist", "cursor->curr == NULL", curs);
	return p64_ll_progerror;
    }
    //Current element becomes predecessor
    p64_linklist_t *pred = curs->curr;
    //'curr' is next element
    p64_linklist_t *curr = atomic_load_ptr(&pred->next, __ATOMIC_ACQUIRE);
    while (REM_MARK(curr) != NULL)
    {
	//Not end of list
	//We don't care whether 'pred'ecessor is marked for removal so remove any mark
	curr = REM_MARK(curr);
	//Read next pointer which also returns status of 'curr'
	p64_linklist_t *next = atomic_load_ptr(&curr->next, __ATOMIC_ACQUIRE);
	if (HAS_MARK(next))
	{
	    //'curr' marked for removal, we must remove it
	    next = REM_MARK(next);
	    if (atomic_compare_exchange_ptr(&pred->next,
					    &curr,//Updated on failure
					    next,
					    __ATOMIC_RELEASE,
					    __ATOMIC_RELAXED))
	    {
		//'curr' removed, 'pred' now points to 'next'
		//Keep 'pred'
		curr = next;
		continue;
	    }
	    else //Failed to remove 'curr'
	    {
		return p64_ll_predmark;//Possibly not entirely accurate
	    }
	}
	else //'curr' is not marked for removal
	{
	    //Return 'curr'
	    curs->curr = curr;
	    return p64_ll_success;
	}
    }
    //Reached end of list
    curs->curr = NULL;
    return p64_ll_success;
}

p64_linklist_status_t
p64_linklist_remove(p64_linklist_t *pred,
		    p64_linklist_t *elem)
{
    do
    {
	p64_linklist_t *this = atomic_load_ptr(&pred->next, __ATOMIC_ACQUIRE);
	//Check if predecessor element is marked for REMOVAL
	if (HAS_MARK(this))
	{
	    //Yes, it must first be removed but we can't do that here
	    return p64_ll_predmark;
	}
	//Check if predecessor still points to us
	else if (this == elem)
	{
	    //Found our element, now remove it
	    //Set REMOVE mark on our next pointer (it may already be set)
	    p64_linklist_t *next = atomic_fetch_or(&this->next, MARK_REMOVE, __ATOMIC_RELAXED);
	    //Now try to unlink us
	    if (atomic_compare_exchange_ptr(&pred->next,
					    &this,
					    REM_MARK(next),
					    __ATOMIC_RELAXED,
					    __ATOMIC_RELAXED))
	    {
		//Success, 'this' element is removed
		return p64_ll_success;
	    }
	    //Else CAS failed
	    //Either some other thread helped remove our element
	    //Or predecessor element was also marked for removal and must be removed first
	    //Or some other element was inserted after predecessor element (before us)
	    //Try again with fresh value
	    continue;
	}
	//Else not our element, continue search
	pred = this;
    }
    while (pred != NULL);
    //'elem' not found in list
    return p64_ll_notfound;
}

p64_linklist_status_t
p64_linklist_insert(p64_linklist_t *pred,
		    p64_linklist_t *elem)
{
    if (UNLIKELY(elem == NULL))
    {
	report_error("linklist", "insert NULL element", elem);
	return p64_ll_progerror;
    }
    if (UNLIKELY(HAS_MARK(elem)))
    {
	report_error("linklist", "element has low bit set", elem);
	return p64_ll_progerror;
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
		//No element after 'pred', insertion fails
		return p64_ll_predmark;
	    }
	    pred = next;
	    continue;
	}
	//'next' is not marked for removal
	//Try to swing in new element
	elem->next = next;
	if (atomic_compare_exchange_ptr(&pred->next,
					&next,//Updated on failure
					elem,
					__ATOMIC_RELEASE,
					__ATOMIC_ACQUIRE))
	{
	    //Success inserting new element
	    return p64_ll_success;
	}
	//Else pred->next changed
	//Either 'pred' is being removed or some other element was inserted
	//Restart with updated value of 'next'
    }
}
