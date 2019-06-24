//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "p64_hazardptr.h"
#undef p64_hazptr_acquire
#include "build_config.h"
#include "os_abstraction.h"

#define HAS_QSBR(ptr)          ((uintptr_t)(ptr) & 1)
#define SET_QSBR(ptr) ((void *)((uintptr_t)(ptr) | 1))
#define CLR_QSBR(ptr) ((void *)((uintptr_t)(ptr) & ~(uintptr_t)1))

#include "arch.h"
#include "lockfree.h"
#include "common.h"

#include "thr_idx.h"
#define PRIVATE
#include "p64_qsbr.c"
#define object hp_object
#define thread_state hp_thread_state
#define TS hp_TS
#define alloc_ts hp_alloc_ts
#define garbage_collect hp_garbage_collect
#define userptr_t hp_userptr_t

static inline uint32_t
bitmask(uint32_t n)
{
    return n < 32 ? (1U << n) - 1U : ~(uint32_t)0;
}

static void
eprintf_not_registered(void)
{
    fprintf(stderr, "hazardptr: p64_hazptr_register() not called\n");
    fflush(stderr);
    abort();
}

#define IS_NULL_PTR(ptr) ((uintptr_t)(ptr) < CACHE_LINE)

typedef void *userptr_t;

struct hazard_pointer
{
    userptr_t ref;
};

static inline uint32_t
roundup(uint32_t n)
{
    return ROUNDUP(n, CACHE_LINE / sizeof(struct hazard_pointer));
}

struct p64_hpdomain
{
    uint32_t nrefs;//Number of references per thread
    uint32_t maxobjs;
    uint32_t high_wm;//High watermark of thread index
    struct hazard_pointer hp[] ALIGNED(CACHE_LINE);
};

p64_hpdomain_t *
p64_hazptr_alloc(uint32_t maxobjs, uint32_t nrefs)
{
    if (nrefs == 0)
    {
	void *qsbr = p64_qsbr_alloc(maxobjs);
	if (qsbr != NULL)
	{
	    return SET_QSBR(qsbr);
	}
	return NULL;
    }
    if (nrefs < 1 || nrefs > 32)
    {
	fprintf(stderr, "hazardptr: Invalid number of references\n");
	fflush(stderr);
	abort();
    }
    uint32_t nrefs_rounded = roundup(nrefs);
    size_t nbytes = sizeof(p64_hpdomain_t) +
		    nrefs_rounded * MAXTHREADS * sizeof(struct hazard_pointer);
    p64_hpdomain_t *hpd = p64_malloc(nbytes, CACHE_LINE);
    if (hpd != NULL)
    {
	hpd->nrefs = nrefs;
	hpd->maxobjs = maxobjs;
	hpd->high_wm = 0;
	for (uint32_t i = 0; i < nrefs_rounded * MAXTHREADS; i++)
	{
	    hpd->hp[i].ref = NULL;
	}
	return hpd;
    }
    return NULL;
}

void
p64_hazptr_free(p64_hpdomain_t *hpd)
{
    if (HAS_QSBR(hpd))
    {
	p64_qsbr_free(CLR_QSBR(hpd));
	return;
    }
    uint32_t nrefs_rounded = roundup(hpd->nrefs);
    uint32_t nthreads = __atomic_load_n(&hpd->high_wm, __ATOMIC_ACQUIRE);
    for (uint32_t t = 0; t < nthreads; t++)
    {
	for (uint32_t i = 0; i < hpd->nrefs; i++)
	{
	    if (hpd->hp[t * nrefs_rounded + i].ref != NULL)
	    {
		fprintf(stderr, "hazardptr: References still present\n");
		fflush(stderr);
		abort();
	    }
	}
    }
    p64_mfree(hpd);
}

struct object
{
    userptr_t ptr;
    void (*cb)(userptr_t);
};

//File & line annotation for debugging
struct file_line
{
    const char *file;
    uintptr_t line;
};

struct thread_state
{
    p64_hpdomain_t *hpd;
    uint32_t idx;//Thread index
    uint32_t free;
    uint32_t nrefs;
    struct hazard_pointer *hp;//Ptr to actual hazard pointer array
    struct file_line *fl;
    //Removed but not yet reclaimed objects
    uint32_t nobjs;
    uint32_t maxobjs;
    struct object objs[];
    //File&line array follows
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

    size_t nbytes = sizeof(struct thread_state) +
		    hpd->maxobjs * sizeof(struct object) +
		    hpd->nrefs * sizeof(struct file_line);
    struct thread_state *ts = p64_malloc(nbytes, CACHE_LINE);
    if (ts == NULL)
    {
	perror("malloc"), exit(EXIT_FAILURE);
    }
    uint32_t nrefs_rounded = roundup(hpd->nrefs);
    ts->hpd = hpd;
    ts->idx = idx;
    ts->free = bitmask(hpd->nrefs);
    ts->nrefs = hpd->nrefs;
    ts->hp = &hpd->hp[idx * nrefs_rounded];
    ts->fl = (void *)&ts->objs[hpd->maxobjs];
    ts->nobjs = 0;
    ts->maxobjs = hpd->maxobjs;
    for (uint32_t i = 0; i < hpd->nrefs; i++)
    {
	ts->fl[i].file = NULL;
	ts->fl[i].line = 0;
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
	eprintf_not_registered();
    }
    if (HAS_QSBR(TS))
    {
	p64_qsbr_reactivate();
	return;
    }
    //Nothing to do
}

void
p64_hazptr_register(p64_hpdomain_t *hpd)
{
    if (UNLIKELY(TS == NULL))
    {
	if (HAS_QSBR(hpd))
	{
	    p64_qsbr_register(CLR_QSBR(hpd));
	    TS = SET_QSBR(NULL);
	    return;
	}
	TS = alloc_ts(hpd);
    }
    p64_hazptr_reactivate();
}

void
p64_hazptr_deactivate(void)
{
    if (UNLIKELY(TS == NULL))
    {
	eprintf_not_registered();
    }
    if (HAS_QSBR(TS))
    {
	p64_qsbr_deactivate();
	return;
    }
    //Nothing to do
    if (TS->free != bitmask(TS->nrefs))
    {
	fprintf(stderr, "hazardptr: Thread has allocated hazard pointers\n");
	fflush(stderr);
	abort();
    }
}

void
p64_hazptr_unregister(void)
{
    if (UNLIKELY(TS == NULL))
    {
	eprintf_not_registered();
    }
    if (HAS_QSBR(TS))
    {
	p64_qsbr_unregister();
	TS = NULL;
	return;
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
    p64_mfree(TS);
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
    if (UNLIKELY(TS == NULL))
    {
	eprintf_not_registered();
    }
    uint32_t idx = hp - &TS->hp[0].ref;
    if (UNLIKELY(idx >= TS->nrefs))
    {
	fprintf(stderr, "Invalid hazard pointer %p\n", hp);
	fflush(stderr);
	abort();
    }
    assert(IS_NULL_PTR(*hp));
    if (UNLIKELY((TS->free & (1U << idx)) != 0))
    {
	fprintf(stderr, "Hazard pointer %p already free\n", hp);
	fflush(stderr);
	abort();
    }
    TS->free |= 1U << idx;
    TS->fl[idx].file = NULL;
    TS->fl[idx].line = 0;
    //printf("hazptr_free(%u)\n", idx);
}

inline void
p64_hazptr_annotate(p64_hazardptr_t hp,
		    const char *file,
		    unsigned line)
{
    if (UNLIKELY(TS == NULL))
    {
	eprintf_not_registered();
    }
    if (HAS_QSBR(TS))
    {
	return;
    }
    if (hp != P64_HAZARDPTR_NULL)
    {
	uint32_t idx = hp - &TS->hp[0].ref;
	if (UNLIKELY(idx >= TS->nrefs))
	{
	    fprintf(stderr, "Invalid hazard pointer %p\n", hp);
	    fflush(stderr);
	    abort();
	}
	TS->fl[idx].file = file;
	TS->fl[idx].line = line;
    }
}

uint32_t
p64_hazptr_dump(FILE *fp)
{
    if (UNLIKELY(TS == NULL))
    {
	eprintf_not_registered();
    }
    if (HAS_QSBR(TS))
    {
	return 0;
    }
    for (uint32_t i = 0; i < TS->nrefs; i++)
    {
	if ((TS->free & (1U << i)) == 0)
	{
	    fprintf(fp, "hp[%p]=%p", &TS->hp[i].ref, TS->hp[i].ref);
	    if (TS->fl[i].file != NULL)
	    {
		fprintf(fp, " @ %s:%"PRIu64, TS->fl[i].file, TS->fl[i].line);
	    }
	    fputc('\n', fp);
	}
    }
    return __builtin_popcount(TS->free);
}

static __thread uint32_t _free_hp = ~0;
static __thread void *_hp[32];

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
	eprintf_not_registered();
    }
    if (HAS_QSBR(TS))
    {
	//Allocate hazard pointer if necessary
	if (*hp == P64_HAZARDPTR_NULL)
	{
	    assert(_free_hp != 0);
	    uint32_t idx = __builtin_ctz(_free_hp);
	    _free_hp &= ~(1U << idx);
	    *hp = &_hp[idx];
	}
	void *ptr = __atomic_load_n(pptr, __ATOMIC_ACQUIRE);
	return ptr;
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
    if (*hp != P64_HAZARDPTR_NULL)
    {
	if (HAS_QSBR(TS))
	{
	    uint32_t idx = _hp - *hp;
	    _free_hp |= 1U << idx;
	    if (_free_hp == ~0U)
	    {
		p64_qsbr_quiescent();
	    }
	    *hp = P64_HAZARDPTR_NULL;
	    return;
	}
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
    if (*hp != P64_HAZARDPTR_NULL)
    {
	if (HAS_QSBR(TS))
	{
	    uint32_t idx = _hp - *hp;
	    _free_hp |= 1U << idx;
	    if (_free_hp == ~0U)
	    {
		p64_qsbr_quiescent();
	    }
	    *hp = P64_HAZARDPTR_NULL;
	    return;
	}
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
    //Cast pointers to uintptr_t to avoid undefined behaviour when doing
    //pointer comparison
    uintptr_t pa = *(uintptr_t const *)ppa;
    uintptr_t pb = *(uintptr_t const *)ppb;
    return pa < pb ? -1 : pa > pb ? 1 : 0;
}

//Collect active references from all threads
static uint32_t
collect_refs(userptr_t refs[],
	     struct hazard_pointer hp[],
	     uint32_t nthreads,
	     uint32_t maxrefs)
{
    uint32_t nrefs = 0;
    uint32_t nrefs_rounded = roundup(maxrefs);
    uint32_t t;
    for (t = 0; t + 1 < nthreads;)
    {
	//Unroll loop to create some memory level parallelism
	userptr_t *hp0 = &hp[t++ * nrefs_rounded].ref;
	userptr_t *hp1 = &hp[t++ * nrefs_rounded].ref;
	ASSUME(maxrefs != 0);
	for (uint32_t i = 0; i < maxrefs; i++)
	{
	    userptr_t ptr0 = __atomic_load_n(hp0, __ATOMIC_RELAXED);
	    userptr_t ptr1 = __atomic_load_n(hp1, __ATOMIC_RELAXED);
	    hp0++;
	    hp1++;
	    if (!IS_NULL_PTR(ptr0))
	    {
		refs[nrefs++] = ptr0;
	    }
	    if (!IS_NULL_PTR(ptr1))
	    {
		refs[nrefs++] = ptr1;
	    }
	}
    }
    //Handle any trailing element
    if (t < nthreads)
    {
	userptr_t *hp0 = &hp[t * nrefs_rounded].ref;
	for (uint32_t i = 0; i < maxrefs; i++)
	{
	    userptr_t ptr0 = __atomic_load_n(hp0, __ATOMIC_RELAXED);
	    hp0++;
	    if (!IS_NULL_PTR(ptr0))
	    {
		refs[nrefs++] = ptr0;
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
	 uint32_t nrefs,
	 userptr_t ptr)
{
    assert((int32_t)nrefs >= 0);
    if (nrefs != 0)
    {
	uint32_t half = nrefs / 2;
	if (ptr < refs[half])
	{
	    return find_ptr(refs, half, ptr);
	}
	else if (ptr > refs[half])
	{
	    assert(nrefs >= half + 1);
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
    PREFETCH_FOR_READ(       &TS->hpd->hp[0]);
    PREFETCH_FOR_READ((char*)&TS->hpd->hp[0] + CACHE_LINE);
    uint32_t numthrs = __atomic_load_n(&TS->hpd->high_wm, __ATOMIC_ACQUIRE);
    uint32_t maxrefs = numthrs * TS->nrefs;
    userptr_t refs[maxrefs];
    //Get sorted list of active references
    uint32_t nrefs = collect_refs(refs, TS->hpd->hp, numthrs, TS->nrefs);
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
//If necessary, perform garbage collection on retired objects
bool
p64_hazptr_retire(void *ptr,
		  void (*cb)(void *ptr))
{
    if (UNLIKELY(TS == NULL))
    {
	eprintf_not_registered();
    }
    if (HAS_QSBR(TS))
    {
	return p64_qsbr_retire(ptr, cb);
    }
    if (UNLIKELY(TS->nobjs == TS->maxobjs))
    {
	if (garbage_collect() == TS->maxobjs)
	{
	    return false;//No space for object
	}
    }
    assert(TS->nobjs < TS->maxobjs);
    uint32_t i = TS->nobjs++;
    TS->objs[i].ptr = ptr;
    TS->objs[i].cb  = cb;
    //The object can be reclaimed when no hazard pointer references the object
    return true;
}

uint32_t
p64_hazptr_reclaim(void)
{
    if (UNLIKELY(TS == NULL))
    {
	eprintf_not_registered();
    }
    if (HAS_QSBR(TS))
    {
	if (_free_hp == ~0U)
	{
	    p64_qsbr_quiescent();
	}
	return p64_qsbr_reclaim();
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
