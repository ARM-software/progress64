//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "p64_hazardptr.h"
#undef p64_hazptr_acquire
#include "build_config.h"

#include "arch.h"
#include "common.h"

#define MAXREFS (MAXTHREADS * MAXHPREFS + 1)//One extra element!

#define IS_NULL_PTR(ptr) ((uintptr_t)(ptr) < CACHE_LINE)

typedef void *userptr_t;

struct hazard_area
{
    uint32_t free;//Which refs are free?
    userptr_t refs[MAXHPREFS];
} ALIGNED(CACHE_LINE);

static struct hazard_area hazard_areas[MAXTHREADS];

struct thread_state
{
    uint16_t tidx;//Thread index
    struct
    {
	uint32_t nitems;
	struct
	{
	    userptr_t ptr;
	    void (*cb)(userptr_t);
	} objs [MAXTHREADS * MAXHPREFS];
    } rlist;//Removed but not yet recycled objects
    struct
    {
	const char *file;
	uintptr_t line;//Unsigned the size of a pointer
    } hp_fileline[MAXHPREFS];
} ALIGNED(CACHE_LINE);

static uint32_t tidx_counter = 0;
static uint32_t numthreads = 0;
static struct thread_state thread_state[MAXTHREADS];
static __thread struct thread_state *TS;

static void
p64_hazardptr_init(void)
{
    uint32_t tidx = __atomic_fetch_add(&tidx_counter, 1, __ATOMIC_RELAXED);
    struct thread_state *ts = &thread_state[tidx];
    TS = ts;
    ts->tidx = tidx;
    ts->rlist.nitems = 0;
    hazard_areas[tidx].free = (1U << MAXHPREFS) - 1U;
    for (uint32_t i = 0; i < MAXHPREFS; i++)
    {
	hazard_areas[tidx].refs[i] = NULL;
	ts->hp_fileline[i].file = NULL;
	ts->hp_fileline[i].line = 0;
    }
    //Increment 'numthreads' in-order since this will release our
    //hazard_area and its hazard pointers
    if (__atomic_load_n(&numthreads, __ATOMIC_RELAXED) != tidx)
    {
	SEVL();
	while (WFE() && LDXR32(&numthreads, __ATOMIC_RELAXED) != tidx)
	{
	    DOZE();
	}
    }
    __atomic_store_n(&numthreads, tidx + 1, __ATOMIC_RELEASE);
}

uint32_t
p64_hazptr_maxrefs(void)
{
    return MAXHPREFS;
}

//Allocate a hazard pointer
//The hazard pointer is not initialised (value should be NULL)
static inline p64_hazardptr_t
p64_hazptr_alloc(void)
{
    if (UNLIKELY(TS == NULL))
    {
	p64_hazardptr_init();
    }
    struct hazard_area *ha = &hazard_areas[TS->tidx];
    if (ha->free != 0)
    {
	uint32_t idx = __builtin_ctz(ha->free);
	ha->free &= ~(1U << idx);
	assert(IS_NULL_PTR(ha->refs[idx]));
	//printf("p64_hazptr_alloc(%u)\n", idx);
	return &ha->refs[idx];
    }
    return P64_HAZARDPTR_NULL;
}

//Free a hazard pointer
//The hazard pointer is not reset (write NULL pointer)
static inline void
p64_hazptr_free(p64_hazardptr_t hp)
{
    struct hazard_area *ha = &hazard_areas[TS->tidx];
    uint32_t idx = hp - ha->refs;
    if (UNLIKELY(idx >= MAXHPREFS))
    {
	fprintf(stderr, "Invalid hazard pointer %p\n", hp), abort();
    }
    assert(IS_NULL_PTR(*hp));
    if (UNLIKELY((ha->free & (1U << idx)) != 0))
    {
	fprintf(stderr, "Hazard pointer %p already free\n", hp), abort();
    }
    ha->free |= 1U << idx;
    TS->hp_fileline[idx].file = NULL;
    TS->hp_fileline[idx].line = 0;
    //printf("p64_hazptr_free(%u)\n", idx);
}

inline void
p64_hazptr_annotate(p64_hazardptr_t hp,
		    const char *file,
		    unsigned line)
{
    if (hp != P64_HAZARDPTR_NULL)
    {
	struct hazard_area *ha = &hazard_areas[TS->tidx];
	uint32_t idx = hp - ha->refs;
	if (idx < MAXHPREFS)
	{
	    TS->hp_fileline[idx].file = file;
	    TS->hp_fileline[idx].line = line;
	}
    }
}

unsigned
p64_hazptr_dump(FILE *fp)
{
    if (UNLIKELY(TS == NULL))
    {
	p64_hazardptr_init();
    }
    struct hazard_area *ha = &hazard_areas[TS->tidx];
    for (uint32_t i = 0; i < MAXHPREFS; i++)
    {
	if ((ha->free & (1U << i)) == 0)
	{
	    fprintf(fp, "hp[%p]=%p", &ha->refs[i], ha->refs[i]);
	    if (TS->hp_fileline[i].file != NULL)
	    {
		fprintf(fp, " @ %s:%lu",
			TS->hp_fileline[i].file,
			TS->hp_fileline[i].line);
	    }
	    fputc('\n', fp);
	}
    }
    return __builtin_popcount(ha->free);
}

//Safely acquire a reference to the object whose pointer is stored at the
//specified location
//Re-use any existing hazard pointer
//Allocate a new hazard pointer if necessary
//Don't free any allocated hazard pointer even if is not used
void *
p64_hazptr_acquire(void **pptr,
		   p64_hazardptr_t *hp)
{
    //Reset any existing hazard pointer
    if (*hp != P64_HAZARDPTR_NULL)
    {
	SMP_RMB();
	__atomic_store_n(*hp, NULL, __ATOMIC_RELAXED);
    }
    for (;;)
    {
	//Step 1: Read location
	void *ptr = __atomic_load_n(pptr, __ATOMIC_RELAXED);
	//All pointers into the zeroth cache line are treated as NULL ptrs
	if (UNLIKELY(IS_NULL_PTR(ptr)))
	{
	    //*hp may be valid
	    return ptr;
	}
	//Speculatively prefetch-for-read as early as possible
	PREFETCH_FOR_READ(ptr);

	//Step 2a: Allocate hazard pointer if necessary
	if (*hp == P64_HAZARDPTR_NULL)
	{
	    *hp = p64_hazptr_alloc();
	    if (UNLIKELY(*hp == P64_HAZARDPTR_NULL))
	    {
		//No more hazard pointers available => programming error
		fprintf(stderr, "Failed to allocate hazard pointer\n");
		p64_hazptr_dump(stderr);
		fflush(stderr);
		abort();
	    }
	}
	//Step 2b: Initialise hazard pointer
	__atomic_store_n(*hp, ptr, __ATOMIC_RELAXED);

	//Step 3: The all-important memory barrier
	SMP_MB();

	//Step 4a: Verify reference
	if (LIKELY(__atomic_load_n(pptr, __ATOMIC_RELAXED) == ptr))
	{
	    return ptr;//Success
	}

	//Step 4b: Reset hazard pointer
	__atomic_store_n(*hp, NULL, __ATOMIC_RELAXED);
    }
}

//Release the reference to the object, assume changes have been made
void
p64_hazptr_release(p64_hazardptr_t *hp)
{
    if (*hp != P64_HAZARDPTR_NULL)
    {
	//Reset hazard pointer
#ifdef USE_DMB
	SMP_MB();
	__atomic_store_n(*hp, NULL, __ATOMIC_RELAXED);
#else
	__atomic_store_n(*hp, NULL, __ATOMIC_RELEASE);
#endif
	//Release hazard pointer
	p64_hazptr_free(*hp);
	*hp = P64_HAZARDPTR_NULL;
    }
}

//Release the reference to the object, no changes have been made
void
p64_hazptr_release_ro(p64_hazardptr_t *hp)
{
    if (*hp != P64_HAZARDPTR_NULL)
    {
	SMP_RMB();//Load-only barrier
	//Reset hazard pointer
	__atomic_store_n(*hp, NULL, __ATOMIC_RELAXED);
	//Release hazard pointer
	p64_hazptr_free(*hp);
	*hp = P64_HAZARDPTR_NULL;
    }
}

//Qsort call-back: compare two pointers
static int
compare_ptr(const void *ppa,
	    const void *ppb)
{
    const void *pa = *(const void * const *)ppa;
    const void *pb = *(const void * const *)ppb;
    //Can't use pointer subtraction because difference might not fit into an int
    return pa < pb ? -1 : pa > pb ? 1 : 0;
}

//Collect active references from all threads
static uint32_t
collect_refs(userptr_t refs[MAXREFS])
{
    uint32_t nrefs = 0;
    for (uint32_t t = 0; t < numthreads; t++)
    {
	for (uint32_t i = 0; i < MAXHPREFS; i++)
	{
	    userptr_t ptr = __atomic_load_n(&hazard_areas[t].refs[i],
					    __ATOMIC_RELAXED);
	    if (!IS_NULL_PTR(ptr))
	    {
		refs[nrefs++] = ptr;
	    }
	}
    }
    if (nrefs >= 2)
    {
	qsort(refs, nrefs, sizeof refs[0], compare_ptr);
    }
    return nrefs;
}

//Check if a specific reference exists in the array
static bool
find_ptr(userptr_t refs[],
	 int nrefs,
	 userptr_t ptr)
{
    if (nrefs > 0)
    {
	int half = nrefs / 2;
	if (ptr < refs[half])
	{
	    return find_ptr(refs, half - 1, ptr);
	}
	else if (ptr > refs[half])
	{
	    return find_ptr(refs + half + 1, nrefs - half - 1, ptr);
	}
	else//ptr == refs[half]
	{
	    return true;
	}
    }
    return false;
}

//Traverse over all retired objects and reclaim those that have no active
//references
static int
garbage_collect(void)
{
    if (UNLIKELY(TS == NULL))
    {
	p64_hazardptr_init();
    }
    struct thread_state *ts = TS;
    uint32_t nreclaimed = 0;
    userptr_t refs[MAXREFS];
    //Get sorted list of active references
    uint32_t nrefs = collect_refs(refs);
    //Traverse list of retired objects
    uint32_t n = 0;
    for (uint32_t i = 0; i != ts->rlist.nitems; i++)
    {
	userptr_t ptr         = ts->rlist.objs[i].ptr;
	void (*cb)(userptr_t) = ts->rlist.objs[i].cb;
	if (find_ptr(refs, nrefs, ptr))
	{
	    //Retired object still referenced, keep it in rlist
	    ts->rlist.objs[n  ].ptr = ptr;
	    ts->rlist.objs[n++].cb  = cb;
	}
	else
	{
	    //No references found to retired object, reclaim it
	    cb(ptr);
	    nreclaimed++;
	}
    }
    //Some objects may remain in the list of retired objects
    ts->rlist.nitems = n;
    return nreclaimed;
}

//Retire an object
//Periodically perform garbage collection on retired objects
void
p64_hazptr_retire(void *ptr,
		  void (*cb)(void *ptr))
{
    struct thread_state *ts = TS;
    //There is always space for one extra retired object
    ts->rlist.objs[ts->rlist.nitems  ].ptr = ptr;
    ts->rlist.objs[ts->rlist.nitems++].cb  = cb;
    if (ts->rlist.nitems == MAXREFS)
    {
	//rlist full
	//Ensure all removals are visible before we read hazard pointers
	SMP_WMB();
	//Try to reclaim objects
	(void)garbage_collect();
	//At least one object must have been reclaimed
	assert(ts->rlist.nitems < MAXREFS);
    }
}

bool
p64_hazptr_reclaim(void)
{
    //Ensure all removals are visible before we read hazard pointers
    SMP_WMB();
    //Try to reclaim objects
    uint32_t nreclaimed = garbage_collect();
    return nreclaimed != 0;
}
