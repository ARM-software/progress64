//Copyright (c) 2024, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <assert.h>
#include <stddef.h>
#include <stdlib.h>

#if defined __aarch64__ && !defined __ARM_FEATURE_ATOMICS
#warning lfstack built without atomics will have poor performance
#endif

#include "p64_lfstack.h"
#include "build_config.h"
#include "arch.h"
#include "err_hnd.h"
#include "atomic.h"

#define TAG_INC 2
#define UPD_IN_PROG 1U

//Read top of stack atomically while setting the update-in-progress flag
static inline
p64_lfstack_t
atomic_load_stk_updprog(p64_lfstack_t *stk, int mo)
{
    p64_lfstack_t old;
    //The tag increments monotonically so if the tag is the same, we know the
    //head pointer hasn't changed
#ifdef __aarch64__
    //Arm specific implementation using address dependencies
    do
    {
#ifdef __ARM_FEATURE_ATOMICS
	old.tag = atomic_fetch_or(&stk->tag, UPD_IN_PROG, mo);
#else
	old.tag = atomic_load_n(&stk->tag, mo);
#undef UPD_IN_PROG
#define UPD_IN_PROG 0U
#endif
	old.head = atomic_load_ptr(addr_dep(&stk->head, old.tag), __ATOMIC_RELAXED);
    }
    while (atomic_load_n(addr_dep(&stk->tag, old.head), __ATOMIC_RELAXED) != (old.tag | UPD_IN_PROG));
#else
    (void)mo;
    do
    {
	old.tag = atomic_fetch_or(&stk->tag, UPD_IN_PROG, __ATOMIC_ACQUIRE);
	old.head = atomic_load_ptr(&stk->head, __ATOMIC_ACQUIRE);
    }
    while (atomic_load_n(&stk->tag, __ATOMIC_RELAXED) != (old.tag | UPD_IN_PROG));
#endif
    return old;
}

void
p64_lfstack_init(p64_lfstack_t *stk)
{
    stk->head = NULL;
    stk->tag = 0;
}

void
p64_lfstack_enqueue(p64_lfstack_t *stk, p64_lfstack_elem_t *elem)
{
    //Must NOT enqueue NULL pointer
    if (UNLIKELY(elem == NULL))
    {
	report_error("lfstack", "enqueue NULL element", 0);
	return;
    }
    for (;;)
    {
	union
	{
	    __int128 pp;
	    p64_lfstack_t st;
	} old, swp;
	//Atomic load of head, set the update-in-progress flag
	old.st = atomic_load_stk_updprog(stk, __ATOMIC_RELAXED);
	elem->next = old.st.head;
	swp.st.head = elem;
	swp.st.tag = (old.st.tag + TAG_INC) & ~UPD_IN_PROG;//Clear the update-in-progress flag
	old.st.tag |= UPD_IN_PROG;//Match value in memory
	//Use an address dependency on the output of atomic_load_stk_updprog() to prevent the load
	//from being speculated
	//This avoids shared copies that interfer with later CAS
	if (atomic_load_n(addr_dep(&stk->tag, old.st.tag), __ATOMIC_RELAXED) != old.st.tag)
	{
	    //Extra-check failed, restart loop
	    continue;
	}
	//A0: write stack top, synchronize with A1
	if (atomic_compare_exchange_n((__int128 *)stk, &old.pp, swp.pp, __ATOMIC_RELEASE, __ATOMIC_RELAXED))
	{
	    //Success, exit loop
	    return;
	}
	//Else CAS failed
    }
}

p64_lfstack_elem_t *
p64_lfstack_dequeue(p64_lfstack_t *stk)
{
    uint32_t ntries = 0;
    for (;;)
    {
	union
	{
	    __int128 pp;
	    p64_lfstack_t st;
	} old, swp;
	//Atomic load of head, set the update-in-progress flag
	//A1: read stack top, synchronize with A0
	old.st = atomic_load_stk_updprog(stk, __ATOMIC_ACQUIRE);
	if (UNLIKELY(old.st.head == NULL))
	{
	    return NULL;
	}
	//Check update-in-progress flag
	if ((old.st.tag & UPD_IN_PROG) != 0 && (++ntries & 1) != 0)
	{
	    //Update-in-progress flag set, backoff
	    for (uint32_t i = 0; i < 2000; i++)
	    {
		doze();
	    }
	    //Start from beginning
	    continue;
	}
	//Else update-in-progress flag not set or we will go ahead anyway
	//Dereferencing head, likely cache miss, this prolongs the critical section
	swp.st.head = old.st.head->next;
	//Increment the tag and clear the update-in-progress flag
	swp.st.tag = (old.st.tag + TAG_INC) & ~UPD_IN_PROG;
	//We set the update-in-progress flag when reading head so update old to match memory
	old.st.tag |= UPD_IN_PROG;
	//Perform extra check before CAS in order to avoid (expensive) CAS failure
	if (atomic_load_n(addr_dep(&stk->tag, old.st.tag), __ATOMIC_RELAXED) != old.st.tag)
	{
	    //Extra-check failed, start from beginning
	    continue;
	}
	if (atomic_compare_exchange_n((__int128 *)stk, &old.pp, swp.pp, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
	{
	    //Success
	    return old.st.head;
	}
	//Else CAS failed
    }
}
