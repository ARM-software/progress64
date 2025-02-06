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

static void
linklist_remove(p64_linklist_t *list,
		p64_linklist_t *prev,
		p64_linklist_t *elem)
{
    for (;;)
    {
	p64_linklist_t *this = atomic_load_ptr(&prev->next, __ATOMIC_ACQUIRE);
	//Check if previous node is marked for REMOVAL
	if (HAS_MARK(this))
	{
	    //Yes, this node must first be removed
	    (void)linklist_remove(list, list, prev);
	    //Prev node removed, start from beginning of the list
	    prev = list;
	    continue;
	}
	else if (UNLIKELY(this == NULL))
	{
	    //End of list, element not found
	    return;
	}
	else if (this == elem)
	{
	    //Found our element, now remove it
	    //Set REMOVE mark on our next pointer (it may already be set)
	    p64_linklist_t *nxt = atomic_fetch_or(&this->next, MARK_REMOVE, __ATOMIC_RELAXED);
	    //Now try to unlink us
	    if (atomic_compare_exchange_ptr(&prev->next,
					    &this,
					    REM_MARK(nxt),
					    __ATOMIC_RELAXED,
					    __ATOMIC_RELAXED))
	    {
		//Success, 'this' node is removed
		return;
	    }
	    //Else CAS failed
	    //Either some other thread helped remove our element
	    //Or previous element was also marked for removal and must be removed first
	    //Or some other element was inserted after previous element (before us)
	    prev = list;
	    continue;
	}
	//Else not our element, continue search
	prev = this;
    }
}

void
p64_linklist_remove(p64_linklist_t *list,
		    p64_linklist_t *elem)
{
    //Since we don't know our previous element, we need to start from the beginning
    linklist_remove(list, list, elem);
}

void
p64_linklist_insert(p64_linklist_t *list,
		    p64_linklist_t *prev,
		    p64_linklist_t *elem)
{
    if (UNLIKELY(elem == NULL))
    {
	report_error("linklist", "insert NULL element", elem);
	return;
    }
    if (UNLIKELY(HAS_MARK(elem)))
    {
	report_error("linklist", "element has low bit set", elem);
	return;
    }
    for (;;)
    {
	p64_linklist_t *nxt = atomic_load_ptr(&prev->next, __ATOMIC_ACQUIRE);
	if (HAS_MARK(nxt))
	{
	    //Previous element is marked for deletion
	    //Give a helping hand
	    //Since we don't know prev of previous element, we need to start from the beginning
	    (void)linklist_remove(list, prev, REM_MARK(nxt));
	    //TODO: If previous element was removed, can we insert before prev->next?
	    //Insert at beginning of list
	    prev = list;
	    continue;
	}
	//'nxt' is not marked for removal
	//Try to swing in new element
	elem->next = nxt;
	if (atomic_compare_exchange_ptr(&prev->next, &nxt, elem, __ATOMIC_RELEASE, __ATOMIC_RELAXED))
	{
	    //Success inserting new element
	    return;
	}
	//Else prev->next changed, either 'prev' is being removed or some other element was inserted
	//Retry the update
    }
}

p64_linklist_t *
p64_linklist_traverse(p64_linklist_t *list,
		      p64_linklist_trav_cb cb,
		      const void *arg)
{
    p64_linklist_t *result = NULL;
    p64_linklist_t *prev = list;
    for (;;)
    {
	p64_linklist_t *this = atomic_load_ptr(&prev->next, __ATOMIC_ACQUIRE);
	//TODO ignore (or remove) elements marked for removal
	this = REM_MARK(this);
	if (this == NULL)
	{
	    break;
	}
	uint32_t flags = cb(arg, this);
	if (flags & P64_LINKLIST_F_RETURN)
	{
	    result = this;
	}
	if (flags & P64_LINKLIST_F_REMOVE)
	{
	    (void)linklist_remove(list, prev, this);
	}
	if (flags & P64_LINKLIST_F_STOP)
	{
	    break;
	}
	prev = this;
    }
    return result;
}
