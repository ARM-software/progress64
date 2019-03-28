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
#include "lockfree.h"
#include "common.h"
#include "thr_idx.h"

static void
eprintf_not_registered(void)
{
    fprintf(stderr, "hazardptr: p64_hazptr_register() not called\n");
    fflush(stderr);
}

#define IS_NULL_PTR(ptr) ((uintptr_t)(ptr) < CACHE_LINE)

typedef void *userptr_t;

struct hazard_pointer
{
    userptr_t ref;
    //File & line annotation for debugging
    const char *file;
    uintptr_t line;
};

static inline uint32_t
hp_to_index(userptr_t *hp, struct hazard_pointer *hp0)
{
    size_t offset = (uintptr_t)hp - (uintptr_t)&hp0->ref;
    //Compiler will generate multiplication instead of division
    return offset / sizeof(struct hazard_pointer);
}

struct p64_hpdomain
{
    uint32_t nrefs;//Number of references per thread
    uint32_t high_wm;//High watermark of thread index
    struct hazard_pointer *hpp[MAXTHREADS];//Ptrs to each threads' hazard pointers
};

p64_hpdomain_t *
p64_hazptr_alloc(uint32_t nrefs)
{
    if (nrefs < 1 || nrefs > 32)
    {
	fprintf(stderr, "hazardptr: Invalid number of references\n");
	fflush(stderr);
	abort();
    }
    size_t nbytes = ROUNDUP(sizeof(p64_hpdomain_t), CACHE_LINE);
    p64_hpdomain_t *hpd = aligned_alloc(CACHE_LINE, nbytes);
    if (hpd != NULL)
    {
	hpd->nrefs = nrefs;
	hpd->high_wm = 0;
	for (uint32_t i = 0; i < MAXTHREADS; i++)
	{
	    hpd->hpp[i] = NULL;
	}
	return hpd;
    }
    return NULL;
}

void
p64_hazptr_free(p64_hpdomain_t *hpd)
{
    uint32_t nthreads = __atomic_load_n(&hpd->high_wm, __ATOMIC_ACQUIRE);
    for (uint32_t t = 0; t < nthreads; t++)
    {
	if (__atomic_load_n(&hpd->hpp[t], __ATOMIC_RELAXED) != NULL)
	{
	    fprintf(stderr, "hazardptr: Registered threads still present\n");
	    fflush(stderr);
	    abort();
	}
    }
    free(hpd);
}

struct object
{
    userptr_t ptr;
    void (*cb)(userptr_t);
};

struct thread_state
{
    p64_hpdomain_t *hpd;
    uint32_t idx;//Thread index
    uint32_t free;
    uint32_t nrefs;
    struct hazard_pointer *hp;//Ptr to actual hazard pointer array
    //Removed but not yet reclaimed objects
    uint32_t nobjs;
    uint32_t maxobjs;
    struct object objs[];
} ALIGNED(CACHE_LINE);

static __thread struct thread_state *TS = NULL;

static struct thread_state *
alloc_ts(p64_hpdomain_t *hpd)
{
    assert(TS == NULL);
    //Attempt to allocate a thread index
    int32_t idx = p64_idx_alloc();
    if (idx < 0)
    {
	fprintf(stderr, "hazardptr: Too many registered threads\n");
	fflush(stderr);
	abort();
    }

    uint32_t maxobjs = hpd->nrefs * MAXTHREADS + 1;
    size_t nbytes = ROUNDUP(sizeof(struct thread_state) +
			    maxobjs * sizeof(struct object) +
			    hpd->nrefs * sizeof(struct hazard_pointer),
			    CACHE_LINE);
    struct thread_state *ts = aligned_alloc(CACHE_LINE, nbytes);
    if (ts == NULL)
    {
	perror("malloc"), exit(EXIT_FAILURE);
    }
    ts->hpd = hpd;
    ts->idx = idx;
    ts->free = hpd->nrefs < 32 ? (1U << hpd->nrefs) - 1U : ~(uint32_t)0;
    ts->nrefs = hpd->nrefs;
    ts->hp = (void *)&ts->objs[maxobjs];
    ts->nobjs = 0;
    ts->maxobjs = maxobjs;
    for (uint32_t i = 0; i < hpd->nrefs; i++)
    {
	ts->hp[i].ref = NULL;
	ts->hp[i].file = NULL;
	ts->hp[i].line = 0;
    }
    //Conditionally update high watermark of indexes
    lockfree_fetch_umax_4(&hpd->high_wm, (uint32_t)idx + 1, __ATOMIC_RELAXED);
    return ts;
}

void
p64_hazptr_reactivate(void)
{
    if (UNLIKELY(TS == NULL))
    {
	eprintf_not_registered(); abort();
    }
    __atomic_store_n(&TS->hpd->hpp[TS->idx], TS->hp, __ATOMIC_RELAXED);
    //Ensure our hazard pointers are observable before any reads are observed
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

void
p64_hazptr_register(p64_hpdomain_t *hpd)
{
    if (UNLIKELY(TS == NULL))
    {
	TS = alloc_ts(hpd);
    }
    p64_hazptr_reactivate();
}

void
p64_hazptr_deactivate(void)
{
    if (UNLIKELY(TS == NULL))
    {
	eprintf_not_registered(); abort();
    }
    //Mark thread as inactive, no references kept
    __atomic_store_n(&TS->hpd->hpp[TS->idx], NULL, __ATOMIC_RELEASE);
}

void
p64_hazptr_unregister(void)
{
    if (UNLIKELY(TS == NULL))
    {
	eprintf_not_registered(); abort();
    }
    if (TS->nobjs != 0)
    {
	fprintf(stderr, "hazardptr: Thread has %u unreclaimed objects\n",
		TS->nobjs);
	fflush(stderr);
	abort();
    }
    p64_hazptr_deactivate();
    p64_idx_free(TS->idx);
    free(TS);
    TS = NULL;
}

//Allocate a hazard pointer
//The hazard pointer is not initialised (value should be NULL)
static inline p64_hazardptr_t
hazptr_alloc(void)
{
    if (LIKELY(TS->free != 0))
    {
	uint32_t idx = __builtin_ctz(TS->free);
	TS->free &= ~(1U << idx);
	assert(IS_NULL_PTR(TS->hp[idx].ref));
	//printf("hazptr_alloc(%u)\n", idx);
	return &TS->hp[idx].ref;
    }
    return P64_HAZARDPTR_NULL;
}

//Free a hazard pointer
//The hazard pointer is not reset (write NULL pointer)
static inline void
hazptr_free(p64_hazardptr_t hp)
{
    uint32_t idx = hp_to_index(hp, &TS->hp[0]);
    if (UNLIKELY(idx >= TS->nrefs))
    {
	fprintf(stderr, "Invalid hazard pointer %p\n", hp), abort();
    }
    assert(IS_NULL_PTR(*hp));
    if (UNLIKELY((TS->free & (1U << idx)) != 0))
    {
	fprintf(stderr, "Hazard pointer %p already free\n", hp), abort();
    }
    TS->free |= 1U << idx;
    TS->hp[idx].file = NULL;
    TS->hp[idx].line = 0;
    //printf("hazptr_free(%u)\n", idx);
}

inline void
p64_hazptr_annotate(p64_hazardptr_t hp,
		    const char *file,
		    unsigned line)
{
    if (UNLIKELY(TS == NULL))
    {
	eprintf_not_registered(); abort();
    }
    if (hp != P64_HAZARDPTR_NULL)
    {
	uint32_t idx = hp_to_index(hp, &TS->hp[0]);
	if (idx < TS->nrefs)
	{
	    TS->hp[idx].file = file;
	    TS->hp[idx].line = line;
	}
    }
}

uint32_t
p64_hazptr_dump(FILE *fp)
{
    if (UNLIKELY(TS == NULL))
    {
	eprintf_not_registered(); abort();
    }
    for (uint32_t i = 0; i < TS->nrefs; i++)
    {
	if ((TS->free & (1U << i)) == 0)
	{
	    fprintf(fp, "hp[%p]=%p", &TS->hp[i].ref, TS->hp[i].ref);
	    if (TS->hp[i].file != NULL)
	    {
		fprintf(fp, " @ %s:%lu", TS->hp[i].file, TS->hp[i].line);
	    }
	    fputc('\n', fp);
	}
    }
    return __builtin_popcount(TS->free);
}

//Safely acquire a reference to the object whose pointer is stored at the
//specified location
//Re-use any existing hazard pointer
//Allocate a new hazard pointer if necessary
//Don't free any allocated hazard pointer even if is not actually used
void *
p64_hazptr_acquire(void **pptr,
		   p64_hazardptr_t *hp)
{
    if (UNLIKELY(TS == NULL))
    {
	eprintf_not_registered(); abort();
    }
    //Reset any existing hazard pointer
    if (*hp != P64_HAZARDPTR_NULL)
    {
	//Release MO to let any pending stores complete before the reference
	//is abandoned
	__atomic_store_n(*hp, NULL, __ATOMIC_RELEASE);
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
	    *hp = hazptr_alloc();
	    if (UNLIKELY(*hp == P64_HAZARDPTR_NULL))
	    {
		//No more hazard pointers available => programming error
		fprintf(stderr, "Failed to allocate hazard pointer\n");
		p64_hazptr_dump(stderr);
		fflush(stderr);
		abort();
	    }
	}
	//Step 2b: Initialise hazard pointer with reference
	__atomic_store_n(*hp, ptr, __ATOMIC_SEQ_CST);

	//Sequential consistency will separate the store and the load

	//Step 3: Verify reference by re-reading and comparing
	if (LIKELY(__atomic_load_n(pptr, __ATOMIC_SEQ_CST) == ptr))
	{
	    return ptr;//Success
	}

	//Step 4: Lost the race, reset hazard pointer and restart
	__atomic_store_n(*hp, NULL, __ATOMIC_RELAXED);
    }
}

//Release the reference to the object, assume changes have been made
void
p64_hazptr_release(p64_hazardptr_t *hp)
{
    if (UNLIKELY(TS == NULL))
    {
	eprintf_not_registered(); abort();
    }
    if (*hp != P64_HAZARDPTR_NULL)
    {
	//Reset hazard pointer
	__atomic_store_n(*hp, NULL, __ATOMIC_RELEASE);
	//Release hazard pointer
	hazptr_free(*hp);
	*hp = P64_HAZARDPTR_NULL;
    }
}

//Release the reference to the object, no changes have been made
void
p64_hazptr_release_ro(p64_hazardptr_t *hp)
{
    if (UNLIKELY(TS == NULL))
    {
	eprintf_not_registered(); abort();
    }
    if (*hp != P64_HAZARDPTR_NULL)
    {
	smp_fence(LoadStore);//Load-only barrier
	//Reset hazard pointer
	__atomic_store_n(*hp, NULL, __ATOMIC_RELAXED);
	//Release hazard pointer
	hazptr_free(*hp);
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
collect_refs(userptr_t refs[],
	     struct hazard_pointer *vec[],
	     uint32_t nthreads,
	     uint32_t maxrefs)
{
    uint32_t nrefs = 0;
    for (uint32_t t = 0; t < nthreads; t++)
    {
	struct hazard_pointer *hp = __atomic_load_n(&vec[t], __ATOMIC_ACQUIRE);
	if (hp != NULL)
	{
	    for (uint32_t i = 0; i < maxrefs; i++)
	    {
		userptr_t ptr = __atomic_load_n(&hp[i].ref, __ATOMIC_RELAXED);
		if (!IS_NULL_PTR(ptr))
		{
		    refs[nrefs++] = ptr;
		}
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
	    return find_ptr(refs, half, ptr);
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

//Traverse all pending objects and reclaim those that have no references
static uint32_t
garbage_collect(void)
{
    userptr_t refs[TS->maxobjs];
    //Get sorted list of active references
    uint32_t nrefs = collect_refs(refs,
				  TS->hpd->hpp,
				  __atomic_load_n(&TS->hpd->high_wm,
						  __ATOMIC_ACQUIRE),
				  TS->hpd->nrefs);
    //Traverse list of pending objects
    uint32_t nobjs = 0;
    for (uint32_t i = 0; i < TS->nobjs; i++)
    {
	struct object obj = TS->objs[i];
	if (!find_ptr(refs, nrefs, obj.ptr))
	{
	    for (uint32_t j = 0; j < nrefs; j++)
	    {
		assert(refs[j] != obj.ptr);
	    }
	    //No references found to retired object, reclaim it
	    obj.cb(obj.ptr);
	}
	else
	{
	    //Retired object still referenced, keep it in rlist
	    TS->objs[nobjs++] = obj;
	}
    }
    //Some objects may remain in the list of retired objects
    TS->nobjs = nobjs;
    //Return number of remaining unreclaimed objects
    //Caller can compute number of available slots
    return nobjs;
}

//Retire an object
//Periodically perform garbage collection on retired objects
void
p64_hazptr_retire(void *ptr,
		  void (*cb)(void *ptr))
{
    if (UNLIKELY(TS == NULL))
    {
	eprintf_not_registered(); abort();
    }
    //There is always space for one extra retired object
    assert(TS->nobjs < TS->maxobjs);
    TS->objs[TS->nobjs  ].ptr = ptr;
    TS->objs[TS->nobjs++].cb  = cb;
    if (TS->nobjs == TS->maxobjs)
    {
	//Ensure all removals are visible before we read hazard pointers
	smp_fence(StoreLoad);
	//Try to reclaim objects
	(void)garbage_collect();
	//At least one object must have been reclaimed
	assert(TS->nobjs < TS->maxobjs);
    }
}

uint32_t
p64_hazptr_reclaim(void)
{
    if (UNLIKELY(TS == NULL))
    {
	eprintf_not_registered(); abort();
    }
    if (TS->nobjs == 0)
    {
	//Nothing to reclaim
	return 0;
    }
    //Try to reclaim objects
    uint32_t nremaining = garbage_collect();
    return nremaining;
}
