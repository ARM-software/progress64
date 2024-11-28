//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause
//
//Based on some brainstorming with Samuel Lee formerly @ ARM

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "p64_buckrob.h"
#include "build_config.h"
#include "os_abstraction.h"

#include "arch.h"
#include "lockfree.h"
#include "common.h"
#include "err_hnd.h"

#define THE_BUCK (void *)(1U)

struct p64_buckrob
{
    //Constants
    uint32_t mask;
    bool user_acquire;
    p64_buckrob_cb cb;
    void *arg;
    //Written by p64_buckrob_release() (when in-order)
    uint32_t head ALIGNED(CACHE_LINE);
    //Written by p64_buckrob_acquire()
    uint32_t tail ALIGNED(CACHE_LINE);
    //Written by p64_buckrob_release()
    void *ring[] ALIGNED(CACHE_LINE);
};

p64_buckrob_t *
p64_buckrob_alloc(uint32_t nelems,
		 bool user_acquire,
		 p64_buckrob_cb cb,
		 void *arg)
{
    if (nelems < 1 || nelems > 0x80000000)
    {
	report_error("buckrob", "invalid number of elements", nelems);
	return NULL;
    }
    size_t ringsize = ROUNDUP_POW2(nelems);
    size_t nbytes = ROUNDUP(sizeof(p64_buckrob_t) + ringsize * sizeof(void *),
			    CACHE_LINE);
    p64_buckrob_t *rob = p64_malloc(nbytes, CACHE_LINE);
    if (rob != NULL)
    {
	//Clear the metadata
	memset(rob, 0, nbytes);
	rob->mask = ringsize - 1;
	rob->user_acquire = user_acquire;
	rob->cb = cb;
	rob->arg = arg;
	rob->head = 0;
	rob->tail = 0;
	//First ring pointer has the in-order "buck"
	rob->ring[0] = THE_BUCK;
	return rob;
    }
    return NULL;
}

void
p64_buckrob_free(p64_buckrob_t *rob)
{
    if (rob != NULL)
    {
	if (!rob->user_acquire && rob->head != rob->tail)
	{
	    report_error("buckrob", "reorder buffer not empty", rob);
	    return;
	}
	p64_mfree(rob);
    }
}

uint32_t
p64_buckrob_acquire(p64_buckrob_t *rob,
		   uint32_t requested,
		   uint32_t *sn)
{
    uint32_t head, tail;
    int32_t actual;
    PREFETCH_FOR_WRITE(&rob->tail);
    tail = __atomic_load_n(&rob->tail, __ATOMIC_RELAXED);
    do
    {
	head = __atomic_load_n(&rob->head, __ATOMIC_ACQUIRE);
	int32_t available = (rob->mask + 1) - (tail - head);
	//Use signed arithmetic for robustness as head & tail are not read
	//atomically, available may be < 0
	actual = MIN(available, (int32_t)requested);
	if (UNLIKELY(actual <= 0))
	{
	    return 0;
	}
    }
    while (!__atomic_compare_exchange_n(&rob->tail,
					&tail,//Updated on failure
					tail + actual,
					/*weak=*/true,
					__ATOMIC_RELAXED,
					__ATOMIC_RELAXED));
    *sn = tail;
    return actual;
}

#if defined __aarch64__ && !defined __ARM_FEATURE_ATOMICS
static inline void
atomic_snmax_4(uint32_t *loc, uint32_t neu, int mo_store)
{
    uint32_t old = __atomic_load_n(loc, __ATOMIC_RELAXED);
    do
    {
	if ((int32_t)(neu - old) <= 0)//neu <= old
	{
	    return;
	}
	//Else neu > old, update *loc
    }
    while (!__atomic_compare_exchange_n(loc,
					&old,//Updated on failure
					neu,
					/*weak=*/true,
					mo_store,
					__ATOMIC_RELAXED));
}
#endif

static inline bool
AFTER(uint32_t x, uint32_t y)
{
    return (int32_t)((x) - (y)) > 0;
}

void
p64_buckrob_release(p64_buckrob_t *rob,
		    uint32_t sn,
		    void *elems[],
		    uint32_t nelems)
{
    if (UNLIKELY(nelems == 0))
    {
	return;
    }

    uint32_t mask = rob->mask;
    p64_buckrob_cb cb = rob->cb;
    void *arg = rob->arg;
    if (rob->user_acquire)
    {
	//With user_acquire, the user might have been generous and allocated
	//a SN currently outside of the ROB window
	uint32_t sz = mask + 1;
	if (UNLIKELY(AFTER(sn + nelems,
			   __atomic_load_n(&rob->head, __ATOMIC_ACQUIRE) + sz)))
	{
	    //We must wait for enough elements to be retired so that our SN
	    //fits inside the ROB window
	    while (AFTER(sn + nelems, LDX(&rob->head, __ATOMIC_ACQUIRE) + sz))
	    {
		WFE();
	    }
	}
    }
    else if (UNLIKELY(AFTER(sn + nelems, rob->tail)))
    {
	report_error("buckrob", "invalid sequence number", sn + nelems);
	return;
    }

    //Store all but first element in ring
    for (uint32_t i = 1; i < nelems; i++)
    {
	assert(elems[i] != NULL && elems[i] != P64_BUCKROB_RESERVED_ELEM);
	__atomic_store_n(&rob->ring[(sn + i) & mask],
			 elems[i],
			 __ATOMIC_RELAXED);
    }

    void *elem = elems[0];
    assert(elem != NULL && elem != P64_BUCKROB_RESERVED_ELEM);
    //Assume not in-order <=> out-of-order
    //Attempt to release our (first) element
    void *old = NULL;
    if (__atomic_compare_exchange_n(&rob->ring[sn & mask],
				    &old,
				    elem,
				    /*weak=*/false,
				    __ATOMIC_ACQ_REL,
				    __ATOMIC_ACQUIRE))
    {
	//Success, out-of-order release
	return;
    }
    //Else failure - we have acquired the buck
    assert(old == THE_BUCK);
    //We are now in-order and responsible for retiring elements
    uint32_t npending = 0;
    uint32_t org_sn = sn;
    //Free our slot
    __atomic_store_n(&rob->ring[sn & mask], NULL, __ATOMIC_RELAXED);
    cb(arg, elem, sn++);
    npending++;
    //Read next slot
    elem = __atomic_load_n(&rob->ring[sn & mask], __ATOMIC_ACQUIRE);
    do
    {
	//Find valid elements
	while (elem != NULL)
	{
	    //Free this slot
	    __atomic_store_n(&rob->ring[sn & mask], NULL, __ATOMIC_RELAXED);
	    cb(arg, elem, sn++);
	    npending++;
	    //Read next slot
	    elem = __atomic_load_n(&rob->ring[sn & mask], __ATOMIC_ACQUIRE);
	}
	//No more consecutive valid elements
	if (LIKELY(npending != 0))
	{
	    cb(arg, NULL, sn);//'sn' one beyond last reported
	    npending = 0;
	}
	assert(elem == NULL);
	//Mark next slot as in order - pass the buck
    }
    while (!__atomic_compare_exchange_n(&rob->ring[sn & mask],
				        &elem,//Updated on failure
				        THE_BUCK,
				        /*weak=*/true,
				        __ATOMIC_ACQ_REL,
				        __ATOMIC_ACQUIRE));
    //Finally make all freed slots available for new acquisition
#if defined __aarch64__ && !defined __ARM_FEATURE_ATOMICS
    //For targets without atomic add to memory, e.g. ARMv8.0
    atomic_snmax_4(&rob->head, sn, __ATOMIC_RELEASE);
    (void)org_sn;
#else
    //For targets with atomic add to memory, e.g. ARMv8.1
    __atomic_fetch_add(&rob->head, sn - org_sn, __ATOMIC_RELEASE);
#endif
}
