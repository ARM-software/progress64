//Copyright (c) 2020, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

//Non-blocking CAS-N
//See "A practical multi-word compare-and-swap" by Harris, Fraser & Pratt

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "p64_hazardptr.h"
#include "p64_qsbr.h"
#include "p64_mcas.h"

#include "build_config.h"
#include "os_abstraction.h"
#include "common.h"
#include "err_hnd.h"
#include "arch.h"

#ifdef __aarch64__DONTUSE
#include "ldxstx.h"
#define USE_LLSC
#endif

static inline p64_mcas_ptr_t
atomic_load_acquire(p64_mcas_ptr_t *pptr,
		    p64_hazardptr_t *hp,
		    uintptr_t mask,
		    bool use_hp)
{
    if (use_hp)
    {
	return p64_hazptr_acquire_mask(pptr, hp, mask);
    }
    else
    {
	return __atomic_load_n(pptr, __ATOMIC_ACQUIRE);
    }
}

static inline void
atomic_ptr_release(p64_hazardptr_t *hp, bool use_hp)
{
    if (use_hp)
    {
	p64_hazptr_release(hp);
    }
}

//Use 2 lsb to mark our descriptors
#define CCAS_BIT UINT64_C(1)
#define MCAS_BIT UINT64_C(2)
#define DESC_BITS (CCAS_BIT | MCAS_BIT)
#define IS_DESC(ptr) (((uintptr_t)(ptr) & DESC_BITS) != 0)
#define CLR_DESC(ptr) (void *)((uintptr_t)(ptr) & ~DESC_BITS)
#define IS_CCAS_DESC(ptr) (((uintptr_t)(ptr) & CCAS_BIT) != 0)
#define IS_MCAS_DESC(ptr) (((uintptr_t)(ptr) & MCAS_BIT) != 0)
#define SET_CCAS_DESC(ptr) (void *)((uintptr_t)(ptr) | CCAS_BIT)
#define SET_MCAS_DESC(ptr) (void *)((uintptr_t)(ptr) | MCAS_BIT)

struct ccas_desc
{
    p64_mcas_ptr_t *loc;
    p64_mcas_ptr_t exp, new;
};

struct mcas_desc
{
    uint8_t maxn;
    uint8_t n;
    uint8_t status;
    struct mcas_desc *next;
    struct ccas_desc ccas[];
};

enum status
{
    UNDECIDED = 0, SUCCESS, FAILURE
};

static inline uint32_t
find_ccas_idx(const struct mcas_desc *md, p64_mcas_ptr_t loc)
{
    for (uint32_t i = 0; i < md->n; i++)
    {
	if (md->ccas[i].loc == loc)
	{
	    return i;
	}
    }
    fprintf(stderr, "mcas: corrupt MCAS descriptor %p\n", md);
    abort();
}

static inline void
ccas_help(struct mcas_desc *md, uint32_t i)
{
    assert(!IS_DESC(md));
    struct ccas_desc *cd = &md->ccas[i];
    p64_mcas_ptr_t exp = SET_CCAS_DESC(md);
    if (__atomic_load_n(&md->status, __ATOMIC_ACQUIRE) == UNDECIDED)
    {
	//Attempt to replace CCAS descriptor with MCAS descriptor
	__atomic_compare_exchange_n(cd->loc, &exp, SET_MCAS_DESC(md),
				    /*weak=*/false,
				    __ATOMIC_RELEASE,
				    __ATOMIC_RELAXED);
    }
    else //Roll back to original value
    {
	__atomic_compare_exchange_n(cd->loc, &exp, cd->exp,
				    /*weak=*/false,
				    __ATOMIC_RELAXED,
				    __ATOMIC_RELAXED);
    }
}

#ifdef USE_LLSC
static inline p64_mcas_ptr_t
ccas(p64_mcas_ptr_t *loc,
     const p64_mcas_ptr_t exp,
     const p64_mcas_ptr_t new,
     uint8_t *status,
     p64_hazardptr_t *hpp)
{
    if (hpp != NULL)
    {
	for (;;)
	{
	    p64_mcas_ptr_t old = p64_hazptr_acquire_mask(loc, hpp, ~DESC_BITS);
	    //'old' saved in hazard pointer
	    if (UNLIKELY(old != exp))
	    {
		return old;//HP OK
	    }
	    //'old' equals 'exp'
	    uint32_t st;
	    //Exclusives section with a memory read in it...
	    //Need inline asm to avoid stack accesses in exclusives section
	    __asm volatile("2: ldaxr %0,[%2];"//Read *loc
			   "   ldrb %w1,[%4];"//Read *status
			   "   tst %w1,#0xff;"//*status == UNDECIDED ?
			   "   ccmp %3,%0,#0x0,eq;"//old == exp ?
			   "   bne 1f;"//No
			   "   stlxr %w1,%5,[%2];"//Update *loc
			   "   cbnz %w1,2b;"//Retry on failure
			   "1:"
			   : "=r" (old), "=r" (st)
			   : "r" (loc), "r" (exp), "r" (status), "r" (new)
			   : "memory");
	    if (LIKELY(old == exp))
	    {
		//'old' still equals 'exp' => HP OK
		//Status changed or CCAS succeeded
		return old;//HP OK
	    }
	    //Else 'old' differs from 'exp' => HP not OK!
	    //Restart from beginning so we can re-acquire hazard pointer
	}
    }
    else//QSBR
    {
	p64_mcas_ptr_t old;
	do
	{
	    old = ldxptr(loc, __ATOMIC_ACQUIRE);
	    uint32_t st = __atomic_load_n(status, __ATOMIC_RELAXED);
	    if (UNLIKELY(old != exp || st != UNDECIDED))
	    {
		//Not expected value or status changed
		return old;
	    }
	}
	while (UNLIKELY(stxptr(loc, new, __ATOMIC_RELEASE)));
	return old;
    }
}
#else
static inline p64_mcas_ptr_t
ccas(struct mcas_desc *md,
     uint32_t i,
     p64_hazardptr_t *hpp)
{
    struct ccas_desc *cd = &md->ccas[i];
    for (;;)
    {
	p64_mcas_ptr_t old = cd->exp;
	if (hpp != NULL)
	{
	    old = p64_hazptr_acquire_mask(cd->loc, hpp, ~DESC_BITS);
	}
	//'old' saved in hazard pointer
	if (LIKELY(old == cd->exp))
	{
	    //'old' equals 'exp', go ahead with CAS
	    if (__atomic_compare_exchange_n(cd->loc,
					    &old,
					    SET_CCAS_DESC(md),
					    /*weak=*/0,
					    __ATOMIC_ACQ_REL,
					    __ATOMIC_ACQUIRE))
	    {
		//CAS succeeded, complete our update
		ccas_help(md, i);
		return old;
	    }
	    //Else CAS failed, 'old' updated
	}
	//Else comparison failed, did not find expected value
	if (!IS_CCAS_DESC(old))
	{
	    return old;
	}
	//Found CCAS descriptor - must help
	struct mcas_desc *alien = CLR_DESC(old);
	ccas_help(alien, find_ccas_idx(alien, cd->loc));
    }
}
#endif

static inline p64_mcas_ptr_t
ccas_read(p64_mcas_ptr_t *loc,
	  p64_hazardptr_t *hpp)
{
    for (;;)
    {
	p64_mcas_ptr_t val =
	    atomic_load_acquire(loc, hpp, ~DESC_BITS, hpp != NULL);
	if (!IS_CCAS_DESC(val))
	{
	    return val;
	}
	//Found CCAS descriptor - must help
	struct mcas_desc *alien = CLR_DESC(val);
	ccas_help(alien, find_ccas_idx(alien, loc));
    }
}

static bool
mcas_help(struct mcas_desc *md, bool use_hp)
{
    //'md' is already protected by a hazard pointer
    p64_hazardptr_t hp = P64_HAZARDPTR_NULL;
    enum status status = __atomic_load_n(&md->status, __ATOMIC_ACQUIRE);
    if (status == UNDECIDED)
    {
	p64_hazardptr_t *hpp = use_hp ? &hp : NULL;
	//Phase 1 - claim locations
	for (uint32_t i = 0; i < md->n; i++)
	{
	    //Install descriptor into *md->ccas[i].loc;
	    for (;;)
	    {
#ifdef USE_LLSC
		p64_mcas_ptr_t val = ccas(md->ccas[i].loc, md->ccas[i].exp,
				     SET_MCAS_DESC(md), &md->status, hpp);
#else
		p64_mcas_ptr_t val = ccas(md, i, hpp);
#endif
		if (LIKELY(val == md->ccas[i].exp || val == SET_MCAS_DESC(md)))
		{
		    break;//Partial success, next location
		}
		if (!IS_MCAS_DESC(val))
		{
		    //Some other thread updated the location
		    status = FAILURE;
		    goto update_status;
		}
		//Found other MCAS operation in progress, help it
		assert(IS_MCAS_DESC(val));
		mcas_help(CLR_DESC(val), use_hp);
	    }
	}
	//Complete success
	status = SUCCESS;
update_status: (void)0;
	//Attempt to update operation status
	uint8_t expected = UNDECIDED;
	if (UNLIKELY(!__atomic_compare_exchange_n(&md->status,
						  &expected,//Updated on failure
						  status,
						  /*weak=*/0,
						  __ATOMIC_ACQ_REL,
						  __ATOMIC_ACQUIRE)))
	{
	    //CAS failed, copy actual status from 'expected'
	    status = expected;
	}
	//Release descriptor from other thread
	atomic_ptr_release(hpp, hpp != NULL);
    }
    //Phase 2 - release locations, finalize or roll back
    for (uint32_t i = 0; i < md->n; i++)
    {
	//Push through or roll back all locations
	//Remove MCAS descriptor
	p64_mcas_ptr_t exp = SET_MCAS_DESC(md);
	p64_mcas_ptr_t new = status == SUCCESS ?
				 md->ccas[i].new :
				 md->ccas[i].exp;
	__atomic_compare_exchange_n(md->ccas[i].loc,
				    &exp,
				    new,
				    /*weak=*/0,
				    __ATOMIC_RELEASE,
				    __ATOMIC_RELAXED);
    }
    return status == SUCCESS;
}

static THREAD_LOCAL struct mcas_desc *stash = NULL;

static struct mcas_desc *
alloc_mcas_desc(uint32_t n)
{
    struct mcas_desc *ptr;
    if (LIKELY((ptr = stash) != NULL))
    {
	if (LIKELY(ptr->maxn >= n))
	{
	    stash = ptr->next;
	    return ptr;
	}
	//Stashed descriptor too small
    }
    //Stash is empty
    return NULL;
}

void
p64_mcas_init(uint32_t count, uint32_t n)
{
    for (uint32_t i = 0; i < count; i++)
    {
	struct mcas_desc *ptr =
	    p64_malloc(sizeof(struct mcas_desc) + sizeof(struct ccas_desc) * n,
		       CACHE_LINE);
	if (UNLIKELY(ptr == NULL))
	{
	    report_error("mcas", "failed to pre-allocate MCAS descriptors",
			 count - i);
	    return;
	}
	ptr->maxn = n;
	ptr->next = stash;
	stash = ptr;
    }
}

static void
free_mcas_desc(void *_ptr)
{
    struct mcas_desc *ptr = _ptr;
    ptr->next = stash;
    stash = ptr;
}

void
p64_mcas_fini(void)
{
    struct mcas_desc *ptr;
    //Empty stash
    while (LIKELY((ptr = stash) != NULL))
    {
	stash = ptr->next;
	p64_mfree(ptr);
    }
}

static bool
insert_entry(struct mcas_desc *md, int32_t *n, struct ccas_desc e)
{
    int32_t i;
    //Find position to insert new element at
    for (i = 0; i < *n; i++)
    {
	if (UNLIKELY(e.loc == md->ccas[i].loc))
	{
	    free_mcas_desc(md);
	    report_error("mcas", "duplicate address", e.loc);
	    return false;
	}
	assert(e.loc != md->ccas[i].loc);
	if (e.loc < md->ccas[i].loc)
	{
	    break;
	}
    }
    //Move all later elements to make place for new element
    for (int32_t j = *n - 1; j >= i; j--)
    {
	md->ccas[j + 1] = md->ccas[j];
    }
    //Insert new element at found position
    md->ccas[i] = e;
    (*n)++;
    //Verify result
    for (i = 0; i < (*n) - 1; i++)
    {
	assert(md->ccas[i].loc < md->ccas[i + 1].loc);
    }
    return true;
}

bool
p64_mcas_casn(uint32_t n,
	      p64_mcas_ptr_t *loc[n],
	      p64_mcas_ptr_t exp[n],
	      p64_mcas_ptr_t new[n],
	      bool use_hp)
{
    struct mcas_desc *md = alloc_mcas_desc(n);
    if (UNLIKELY(md == NULL))
    {
	report_error("mcas", "failed to allocate MCAS descriptor", n);
	return false;
    }
    md->status = UNDECIDED;
    md->n = n;
    int32_t nn = 0;
    for (uint32_t i = 0; i < n; i++)
    {
	if (UNLIKELY(IS_DESC(new[i])))
	{
	    free_mcas_desc(md);
	    report_error("mcas", "invalid argument", new[i]);
	    return false;
	}
	if (!insert_entry(md, &nn,
		    (struct ccas_desc){ .loc=loc[i], .exp=exp[i], .new=new[i] }))
	{
	    return false;
	}
    }
    assert((uint32_t)nn == n);
    bool success = mcas_help(md, use_hp);
    if (use_hp)
    {
	while (!p64_hazptr_retire(md, free_mcas_desc))
	{
	    (void)p64_hazptr_reclaim();
	}
    }
    else
    {
	uint32_t tries = 0;
	while (!p64_qsbr_retire(md, free_mcas_desc))
	{
	    (void)p64_qsbr_reclaim();
	    if (tries++ == 0)
	    {
		//First time we reclaim then retry immediately
		continue;
	    }
	    if (tries % 10000 == 0)
	    {
		report_error("mcas", "QSBR reclamation stalled",
			     tries);
	    }
	    doze();
	}
    }
    return success;
}

p64_mcas_ptr_t
p64_mcas_read(p64_mcas_ptr_t *loc, p64_hazardptr_t *hpp, bool help)
{
    for (;;)
    {
	p64_mcas_ptr_t val = ccas_read(loc, hpp);
	if (LIKELY(!IS_MCAS_DESC(val)))
	{
	    //Caller must free any hazard pointer
	    assert(!IS_DESC(val));
	    return val;
	}
	//Found MCAS descriptor
	if (help)
	{
	    //Help alien MCAS operation
	    mcas_help(CLR_DESC(val), hpp != NULL);
	    continue;//Restart
	}
	//Else return old or new value depending on MCAS operation status
	struct mcas_desc *md = CLR_DESC(val);
	uint32_t idx = find_ccas_idx(md, loc);
	p64_mcas_ptr_t *pptr;
	if (__atomic_load_n(&md->status, __ATOMIC_ACQUIRE) == SUCCESS)
	{
	    pptr = &md->ccas[idx].new;
	}
	else//UNDECIDED or FAILURE
	{
	    pptr = &md->ccas[idx].exp;
	}
	if (hpp != NULL)
	{
	    //Need a new hazard pointer, original hazard pointer protects
	    //MCAS descriptor
	    p64_hazardptr_t hp2 = P64_HAZARDPTR_NULL;
	    //Read and publish old/new value from MCAS descriptor
	    p64_mcas_ptr_t ptr = *pptr;
	    p64_hazptr_publish(ptr, &hp2);
	    //Synchronize with p64_hazptr_reclaim (and p64_hazptr_retire)
	    __atomic_thread_fence(__ATOMIC_SEQ_CST);
	    //Verify MCAS descriptor is still present at location
	    if (__atomic_load_n(loc, __ATOMIC_RELAXED) == val)
	    {
		//Release hazard pointer for MCAS descriptor
		p64_hazptr_release_ro(hpp);
		//Return hazard pointer for returned object
		*hpp = hp2;
		return ptr;
	    }
	    //MCAS descriptor removed, hazard pointer invalid
	    p64_hazptr_release_ro(&hp2);
	    continue;//Restart
	}
	else//QSBR
	{
	    return *pptr;
	}

    }
}

bool
p64_mcas_cas1(p64_mcas_ptr_t *loc,
	      p64_mcas_ptr_t exp,
	      p64_mcas_ptr_t new,
	      bool use_hp)
{
    if (UNLIKELY(IS_DESC(new)))
    {
	report_error("mcas", "invalid argument", new);
	return false;
    }
    p64_hazardptr_t hp = P64_HAZARDPTR_NULL;
    p64_mcas_ptr_t old;
    do
    {
	old = p64_mcas_read(loc, use_hp ? &hp : NULL, true);
	if (old != exp)
	{
	    //Wrong value present
	    atomic_ptr_release(&hp, use_hp);
	    return false;
	}
	//Expected value present
	//Attempt atomic CAS
    }
    while (!__atomic_compare_exchange_n(loc,
					&old,
					new,
					/*weak=*/false,
					__ATOMIC_RELEASE,
					__ATOMIC_RELAXED));
    atomic_ptr_release(&hp, use_hp);
    return true;
}
