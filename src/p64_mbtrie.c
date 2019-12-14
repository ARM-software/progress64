//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "p64_mbtrie.h"
#undef p64_mbtrie_lookup
#include "p64_qsbr.h"
#include "build_config.h"

#include "common.h"
#include "arch.h"
#include "lockfree.h"
#include "os_abstraction.h"
#include "err_hnd.h"

#define STR(x) #x
#define STRSTR(x) STR(x)

#define MAX_STRIDES 16
struct p64_mbtrie
{
    //User-defined call-back when element reference counter reaches 0
    p64_mbtrie_free_cb refcnt_zero_cb;
    p64_mbtrie_elem_t *default_pfx;
    void *refcnt_zero_arg;
    uint8_t use_hp;
    uint8_t maxlen;//Max length of prefixes
    uint8_t nstrides;//Number of strides in strides array
    uint8_t strides[MAX_STRIDES + 1];
    void *base[] ALIGNED(CACHE_LINE);
};

//Pointer points to a (sub)-vector of elements
#if __SIZEOF_POINTER__ == 8
#define VECTOR_BIT (UINT64_C(1) << 48)
#else
#define VECTOR_BIT (UINT32_C(1) << 31)
#endif
#define IS_VECTOR(ptr) (((uintptr_t)(ptr) & VECTOR_BIT) != 0)
#define SET_VECTOR(ptr) (void *)((uintptr_t)(ptr) |  VECTOR_BIT)
#define CLR_VECTOR(ptr) (void *)((uintptr_t)(ptr) & ~VECTOR_BIT)

//Some thread is currently checking if sub-vector can be collapsed
#define COLLAPSE_BIT UINT64_C(0)
#define IS_COLLAPSE(ptr) (((uintptr_t)(ptr) & COLLAPSE_BIT) != 0)
#define SET_COLLAPSE(ptr) (void *)((uintptr_t)(ptr) |  COLLAPSE_BIT)
#define CLR_COLLAPSE(ptr) (void *)((uintptr_t)(ptr) & ~COLLAPSE_BIT)

//Some thread is currently holding the mutex for this sub-vector
#define MUTEX_BIT UINT64_C(1)
#define IS_MUTEX(ptr) (((uintptr_t)(ptr) & MUTEX_BIT) != 0)
#define SET_MUTEX(ptr) (void *)((uintptr_t)(ptr) |  MUTEX_BIT)
#define CLR_MUTEX(ptr) (void *)((uintptr_t)(ptr) & ~MUTEX_BIT)

#define ALL_BITS (COLLAPSE_BIT | VECTOR_BIT | (ALIGNMENT - 1))
#define CLR_ALL(ptr) (void *)((uintptr_t)(ptr) & ~ALL_BITS)
#define HAS_ANY(ptr) (((uintptr_t)(ptr) & ALL_BITS) != 0)

#define GET_PFXLEN(ptr) (uint32_t)((uintptr_t)(ptr) % 64 + 1)
#define SET_PFXLEN(ptr, len) (p64_mbtrie_elem_t *)((uintptr_t)(ptr) + (len) - 1)

//We need bits 0..5 to store prefix length of elements
#define ALIGNMENT 64

static inline uint64_t
pfxlen_to_mask(uint32_t len)
{
    //len=0 => mask = 0
    //len=1 => mask = 0x8000000000000000
    //len=2 => mask = 0xC000000000000000
    //len=64 => mask = 0xFFFFFFFFFFFFFFFF
    if (len == 0)
    {
	return UINT64_C(0);
    }
    else if (len == 64)
    {
	return ~UINT64_C(0);
    }
    else
    {
	return ((UINT64_C(1) << len) - 1) << (64 - len);
    }
}

static inline size_t
stride_to_nslots(uint32_t stride)
{
    return (size_t)1 << (stride);
}

static inline size_t
prefix_to_index(uint64_t pfx, uint32_t stride)
{
    size_t mask = stride_to_nslots(stride) - 1;
    return (pfx >> (64 - stride)) & mask;
}

static inline bool
inside(uint64_t pfx_inner,
       uint32_t pfxlen_inner,
       uint64_t pfx_outer,
       uint32_t pfxlen_outer)
{
    uint64_t size_inner = ~pfxlen_to_mask(pfxlen_inner);
    uint64_t size_outer = ~pfxlen_to_mask(pfxlen_outer);
    if (pfx_inner >= pfx_outer &&
	pfx_inner + size_inner <= pfx_outer + size_outer)
    {
	return true;
    }
    return false;
}

static inline void *
atomic_load_acquire(void **pptr,
		    p64_hazardptr_t *hp,
		    uintptr_t mask,
		    bool use_hp)
{
    if (UNLIKELY(use_hp))
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
    if (UNLIKELY(use_hp))
    {
	p64_hazptr_release(hp);
    }
}

static void
traverse(p64_mbtrie_t *mbt,
	 p64_mbtrie_trav_cb cb,
	 void *arg,
	 bool real_refs,
	 uint32_t depth,
	 void **base,
	 uint64_t pfx,
	 uint32_t pfxlen)
{
    uint32_t stride = mbt->strides[depth];
    assert(stride != 0);
    size_t nslots = stride_to_nslots(stride);
    uint32_t sumstride = 0;
    for (uint32_t i = 0; i <= depth; i++)
    {
	sumstride += mbt->strides[i];
    }
    p64_hazardptr_t hp = P64_HAZARDPTR_NULL;
    for (size_t i = 0; i < nslots; i++)
    {
	uint64_t pfx2 = pfx | (i << (64 - sumstride));
	uint32_t pfxlen2 = pfxlen + stride;
	void *ptr = atomic_load_acquire(&base[i], &hp, ~ALL_BITS, mbt->use_hp);
	if (IS_VECTOR(ptr))
	{
	    ptr = CLR_ALL(ptr);
	    assert(mbt->strides[depth + 1] != 0);
	    traverse(mbt, cb, arg, real_refs, depth + 1, ptr, pfx2, pfxlen2);
	}
	else if (ptr != NULL)
	{
	    cb(arg, pfx2, pfxlen2, CLR_ALL(ptr), GET_PFXLEN(ptr));
	}
	else if (!real_refs)
	{
	    //NULL => default_pfx (if any)
	    p64_hazardptr_t hp2 = P64_HAZARDPTR_NULL;
	    void *def_pfx = atomic_load_acquire((void **)&mbt->default_pfx,
						&hp2,
						~0UL,
						mbt->use_hp);
	    if (def_pfx != NULL)
	    {
		cb(arg, pfx2, pfxlen2, def_pfx, 0);
	    }
	    atomic_ptr_release(&hp2, mbt->use_hp);
	}
    }
    atomic_ptr_release(&hp, mbt->use_hp);
}

void
p64_mbtrie_traverse(p64_mbtrie_t *mbt,
                    p64_mbtrie_trav_cb cb,
                    void *arg,
		    bool real_refs)
{
    if (!mbt->use_hp)
    {
	p64_qsbr_acquire();
    }
    if (real_refs)
    {
	p64_hazardptr_t hp = P64_HAZARDPTR_NULL;
	void *def_pfx = atomic_load_acquire((void **)&mbt->default_pfx,
					    &hp,
					    ~0UL,
					    mbt->use_hp);
	if (def_pfx != NULL)
	{
	    cb(arg, 0, 0, def_pfx, 0);
	}
	atomic_ptr_release(&hp, mbt->use_hp);
    }
    traverse(mbt, cb, arg, real_refs, 0, mbt->base, 0, 0);
    if (!mbt->use_hp)
    {
	p64_qsbr_release();
    }
}

#define VALID_FLAGS P64_MBTRIE_F_HP

p64_mbtrie_t *
p64_mbtrie_alloc(const uint8_t *strides,
		 p64_mbtrie_free_cb refcnt_zero_cb,
		 void *refcnt_zero_arg,
		 uint32_t flags)
{
    if (UNLIKELY((flags & ~VALID_FLAGS) != 0))
    {
	report_error("mbtrie", "invalid flags", flags);
	return NULL;
    }
    assert(pfxlen_to_mask(0) == 0);
    assert(pfxlen_to_mask(1) == 0x8000000000000000);
    assert(pfxlen_to_mask(2) == 0xC000000000000000);
    assert(pfxlen_to_mask(32) == 0xFFFFFFFF00000000);
    assert(pfxlen_to_mask(63) == 0xFFFFFFFFFFFFFFFE);
    assert(pfxlen_to_mask(64) == 0xFFFFFFFFFFFFFFFF);
    uint32_t maxlen = 0;
    uint32_t nstrides = 0;
    while (strides[nstrides] != 0)
    {
	maxlen += strides[nstrides++];
	if (strides[nstrides - 1] >= 64 ||
	    nstrides > MAX_STRIDES ||
	    maxlen > 64)
	{
invalid_stride:
	    report_error("mbtrie", "invalid stride config", strides);
	    return NULL;
	}
    }
    if (nstrides == 0)
    {
	goto invalid_stride;
    }
    //Include base level (depth 0) vector in mbtrie object
    size_t nslots = stride_to_nslots(strides[0]);
    size_t nbytes = sizeof(p64_mbtrie_t) + nslots * sizeof(void *);
    p64_mbtrie_t *mbt = p64_malloc(nbytes, ALIGNMENT);
    if (mbt != NULL)
    {
	//Clear everything including all base pointers
	memset(mbt, 0, nbytes);
	mbt->refcnt_zero_cb = refcnt_zero_cb;
	mbt->refcnt_zero_arg = refcnt_zero_arg;
	mbt->use_hp = (flags & P64_MBTRIE_F_HP) != 0;
	mbt->nstrides = nstrides;
	mbt->maxlen = maxlen;
	for (uint32_t i = 0; i < nstrides; i++)
	{
	    mbt->strides[i] = strides[i];
	}
	//Sentinel element
	mbt->strides[nstrides] = 0;
	return mbt;
    }
    return NULL;
}

//Increment reference counter of element
static inline void
increment_refcnt(p64_mbtrie_elem_t *elem, size_t val)
{
    assert(!IS_VECTOR(elem));
    elem = CLR_ALL(elem);
    if (elem != NULL)
    {
	__atomic_fetch_add(&elem->refcnt, val, __ATOMIC_RELAXED);
    }
}

//Decrement reference counter of element and call user-defined call-back
//when reference counter reaches 0
static inline void
decrement_refcnt(p64_mbtrie_t *mbt, p64_mbtrie_elem_t *elem, size_t val)
{
    assert(!IS_VECTOR(elem));
    elem = CLR_ALL(elem);
    if (elem != NULL)
    {
	if (__atomic_sub_fetch(&elem->refcnt, val, __ATOMIC_RELAXED) == 0)
	{
	    mbt->refcnt_zero_cb(mbt->refcnt_zero_arg, elem);
	}
    }
}

//Allocate a vector of size matching the stride of the specified depth
//and initialise all vector slots to the specific element
static void **
alloc_vec(p64_mbtrie_t *mbt,
	  uint32_t depth,
	  p64_mbtrie_elem_t *elem)
{
    assert(depth < mbt->nstrides);
    size_t nslots = stride_to_nslots(mbt->strides[depth]);
    void **vec = p64_malloc(nslots * sizeof(void *), ALIGNMENT);
    if (UNLIKELY(vec == NULL))
    {
	report_error("mbtrie", "malloc failed", mbt);
	return NULL;
    }
    if (UNLIKELY(HAS_ANY(vec)))
    {
	//Need to ensure we don't get a buffer with e.g. the VECTOR bit set
	report_error("mbtrie", "internal error at "__FILE__":"STRSTR(__LINE__),
		     vec);
	p64_mfree(vec);
	return NULL;
    }
    increment_refcnt(elem, nslots);
    for (size_t i = 0; i < nslots; i++)
    {
	vec[i] = elem;
    }
    return vec;
}

//Free a vector (recursively) and decrement the reference counter of all
//referenced elements. Vectors that have been shared are retired before they
//are freed
static void
free_vec(p64_mbtrie_t *mbt,
	 uint32_t depth,
	 void **vec,
	 bool shared)
{
    assert(depth < mbt->nstrides);
    assert(!HAS_ANY(vec));
    size_t nslots = stride_to_nslots(mbt->strides[depth]);
    size_t last = 0;
    //Find ranges of slots with same content
    for (size_t i = last + 1; i < nslots; i++)
    {
	if (vec[i] != vec[last])
	{
	    //Current range ended, free it
	    if (IS_VECTOR(vec[last]))
	    {
		free_vec(mbt, depth + 1, CLR_ALL(vec[last]), shared);
	    }
	    else
	    {
		decrement_refcnt(mbt, vec[last], i - last);
	    }
	    //Start of new range
	    last = i;
	}
    }
    //Free last range
    if (IS_VECTOR(vec[last]))
    {
	free_vec(mbt, depth + 1, CLR_ALL(vec[last]), shared);
    }
    else
    {
	decrement_refcnt(mbt, vec[last], nslots - last);
    }
    if (shared)
    {
	//Vector was shared, it must be safely disposed of
	if (mbt->use_hp)
	{
	    while (!p64_hazptr_retire(vec, p64_mfree))
	    {
		doze();
	    }
	}
	else
	{
	    while (!p64_qsbr_retire(vec, p64_mfree))
	    {
		doze();
	    }
	}
    }
    else
    {
	//Vector was never shared, free immediately
	p64_mfree(vec);
    }
}

//Check if all slots of a vector contain the specified element
static bool
check_vec(p64_mbtrie_t *mbt,
	  uint32_t depth,
	  void **vec,
	  p64_mbtrie_elem_t *elem)
{
    assert(depth < mbt->nstrides);
    assert(!HAS_ANY(vec));
    size_t nslots = stride_to_nslots(mbt->strides[depth]);
    for (size_t i = 0; i < nslots; i++)
    {
	if (vec[i] != elem)
	{
	    return false;
	}
    }
    return true;
}

void
p64_mbtrie_free(p64_mbtrie_t *mbt)
{
    if (mbt != NULL)
    {
	//Iterate over base vector here since it is part of the mbtrie object
	//Start recursion from the individual elements of the base vector
	size_t nslots = stride_to_nslots(mbt->strides[0]);
	for (size_t i = 0; i < nslots; i++)
	{
	    void *ptr = mbt->base[i];
	    if (IS_VECTOR(ptr))
	    {
		free_vec(mbt, 1, CLR_ALL(ptr), true);
	    }
	    else if (ptr != NULL)
	    {
		p64_mbtrie_elem_t *elem = ptr;
		decrement_refcnt(mbt, elem, 1);
	    }
	}
	p64_mfree(mbt);
    }
}

//Compare-and-swap helper
//'old' and 'new' passed by value, no update on failure
static inline bool
cas(void **loc, void *old, void *new, int mo)
{
    return __atomic_compare_exchange_n(loc,//'*loc' updated on success
				       &old,//Local 'old' updated on failure
				       new,
				       /*weak=*/false,
				       MO_LOAD(mo) | MO_STORE(mo),//XXX
				       MO_LOAD(mo));
}

//Attempt to swing a location from 'cur' to 'new', updating reference counters
//of the elements accordingly
static inline bool
swing_slot(p64_mbtrie_t *mbt,
	   void **slotp,
	   void *cur,
	   void *new)
{
    //Increment refcnt of 'new' before making it shared
    increment_refcnt(new, 1);
    if (cas(slotp, cur, new, __ATOMIC_RELEASE))
    {
	//'cur' removed from shared, decrement its refcnt
	decrement_refcnt(mbt, cur, 1);
	return true;
    }
    //CAS failed, undo increment
    decrement_refcnt(mbt, new, 1);
    return false;
}

//Check if sub-vector remains in place
//Clear any collapse-in-progress bit to notify collapser about our updates
static inline bool
check_remains(void **slotp, void *cur)
{
    assert(IS_VECTOR(cur));
    for (;;)
    {
	void *cur2 = __atomic_load_n(slotp, __ATOMIC_RELAXED);
	//Ignore collapse-in-progress bit when comparing pointers
	if (CLR_COLLAPSE(cur2) != CLR_COLLAPSE(cur))
	{
	    return false;//Slot content modified
	}
	//cur2 == cur (ignoring any collapse bits)
	if (!IS_COLLAPSE(cur))
	{
	    //Collapse not in progress
	    return true;//Slot content preserved
	}
	//Collapse-in-progress bit set, clear it, releasing our updates
	if (cas(slotp, cur2, CLR_COLLAPSE(cur2), __ATOMIC_RELEASE))
	{
	    //Collapse aborted
	    return true;//Slot content preserved
	}
	//Else CAS failed
    }
}

static void
update_pfx(p64_mbtrie_t *mbt,
	   uint32_t depth,
	   void **vec,
	   uint64_t pfx,
	   uint32_t pfxlen,
	   const uint32_t org_pfxlen,
	   p64_mbtrie_elem_t *old,
	   p64_mbtrie_elem_t *new);

//Prefix covers complete slot
static void
update_slot(p64_mbtrie_t *mbt,
	    uint32_t depth,
	    void **slotp,
	    uint64_t pfx,
	    uint32_t pfxlen,
	    const uint32_t org_pfxlen,
	    p64_mbtrie_elem_t *old,
	    p64_mbtrie_elem_t *new)
{
    p64_hazardptr_t hp = P64_HAZARDPTR_NULL;
    for (;;)
    {
	void *cur = atomic_load_acquire(slotp, &hp, ~ALL_BITS, mbt->use_hp);
	if (IS_VECTOR(cur))
	{
	    //Update sub-vector
	    update_pfx(mbt, depth, CLR_ALL(cur), pfx, pfxlen, org_pfxlen, old, new);
	    //Verify that sub-vector remains in place
	    smp_fence(StoreLoad);
	    if (!check_remains(slotp, cur))
	    {
		//Sub-vector does not remain, redo our updates
		continue;//Retry
	    }
	    //Sub-vector remains in place, we are done
	    break;//Success
	}
	else if (CLR_ALL(cur) == old)
	{
	    //Remove occurrencies of 'old' and replace with 'new'
	    //Insert our 'new' element in empty (old=NULL) node, or
	    //remove 'old' elements and replace with 'new' (or NULL) elements
	    if (!swing_slot(mbt, slotp, cur, new))
	    {
		continue;//Retry
	    }
	    break;//Success
	}
	//Else slot occupied with something else
	else if (CLR_ALL(cur) != new)
	{
	    if (new != NULL && org_pfxlen >= GET_PFXLEN(cur))
	    {
		//New prefix is longer (more specific) than current prefix
		//Replace current element with new element
		if (!swing_slot(mbt, slotp, cur, new))
		{
		    continue;//Retry
		}
		//Else success
	    }
	    //Else no new prefix or new prefix is shorter (less specific)
	    break;//Success
	}
	else//cur == new
	{
	    break;//Success
	}
    }
    atomic_ptr_release(&hp, mbt->use_hp);
}

//Check if a vector can be collapsed, if so collapse it and replace it with
//the common element (which may be NULL)
static void
collapse_vec(p64_mbtrie_t *mbt,
	     uint32_t depth,
	     void **slotp)
{
    p64_hazardptr_t hp = P64_HAZARDPTR_NULL;
    for (;;)
    {
	void *const cur = atomic_load_acquire(slotp, &hp, ~ALL_BITS, mbt->use_hp);
	if (cur == NULL || !IS_VECTOR(cur))
	{
	    break;//Nothing to do
	}
	//cur != NULL && IS_VECTOR(cur)
	//Pre-check before we acquire mutex and set collapse-in-progress bit
	//Check if sub-vector can be collapsed, this is true if all slots
	//in the sub-vector have the same value (element or NULL)
	void **vec = CLR_ALL(cur);
	p64_mbtrie_elem_t *elem = __atomic_load_n(&vec[0], __ATOMIC_RELAXED);
	if (!check_vec(mbt, depth + 1, vec, elem))
	{
	    //Vector slots do not have the same value
	    break;//Nothing to do
	}
	//Vector can be collapsed
	//We need to serialize collapsers or another collapser could set
	//the collapse-in-progress bit after it was cleared by some thread,
	//defeating the purpose of the collapse-in-progress bit
	if (IS_MUTEX(cur))
	{
	    //Mutex already taken, some other thread is currently trying to
	    //collapse sub-vector
	    break;//Nothing for us to do
	}
	assert(!IS_COLLAPSE(cur));
	//Attempt to acquire mutex and set collapse bit
	if (!cas(slotp, cur, SET_MUTEX(SET_COLLAPSE(cur)), __ATOMIC_ACQUIRE))
	{
	    //CAS failed, someone else took mutex or modified slot
	    break;//Nothing for us to do
	}
	//Redo check now when collapse-in-progress bit is set
	//Any thread which modifies sub-vector will also clear the collapse bit
	elem = __atomic_load_n(&vec[0], __ATOMIC_RELAXED);
	if (!check_vec(mbt, depth + 1, vec, elem))
	{
	    //No, vector cannot be collapsed
	    //Clear mutex and collapse-in-progress bits, releasing mutex
	    __atomic_store_n(slotp, cur, __ATOMIC_RELEASE);
	    break;//Nothing to do
	}
	assert(!IS_VECTOR(elem));
	//Replace sub-vector with element
	increment_refcnt(elem, 1);
	if (!cas(slotp, SET_MUTEX(SET_COLLAPSE(cur)), elem, __ATOMIC_RELAXED))
	{
	    //CAS failed, collapse-in-progress bit cleared by other thread
	    __atomic_store_n(slotp, cur, __ATOMIC_RELEASE);
	    decrement_refcnt(mbt, elem, 1);
	    continue;//Retry
	}
	//CAS success => mutex released, collapse bit cleared
	//Free elements in removed previously shared vector
	free_vec(mbt, depth + 1, vec, true);
	break;//Success
    }
    atomic_ptr_release(&hp, mbt->use_hp);
}

static void
update_vec(p64_mbtrie_t *mbt,
	   uint32_t depth,
	   void **slotp,
	   uint64_t pfx,
	   uint32_t pfxlen,
	   const uint32_t org_pfxlen,
	   p64_mbtrie_elem_t *old,
	   p64_mbtrie_elem_t *new)
{
    //Prefix covers only subset of a slot
    //We need an indirection node pointing to a vector
    //And then insert prefix in subrange of vector slots
    p64_hazardptr_t hp = P64_HAZARDPTR_NULL;
    for (;;)
    {
	void *cur = atomic_load_acquire(slotp, &hp, ~ALL_BITS, mbt->use_hp);
	if (cur == NULL || !IS_VECTOR(cur))
	{
	    //Either empty slot or leaf node
	    //Allocate sub-vector
	    void **vec = alloc_vec(mbt, depth + 1, cur);
	    if (UNLIKELY(vec == NULL))
	    {
		//Memory allocation failed failed
		break;
	    }
	    //Attempt to insert sub-vector as indirection node
	    if (cas(slotp, cur, SET_VECTOR(vec), __ATOMIC_RELEASE))
	    {
		//CAS succeeded, sub-vector inserted
		//Leaf node successfully replaced, one less reference
		decrement_refcnt(mbt, cur, 1);
	    }
	    else
	    {
		//CAS failed, free sub-vector
		free_vec(mbt, depth + 1, vec, false);
	    }
	    continue;//Restart
	}
	//Else cur != NULL && IS_VECTOR(cur))
	//Insert prefix into existing sub-vector
	update_pfx(mbt,
		   depth + 1,
		   CLR_ALL(cur),
		   pfx << mbt->strides[depth],
		   pfxlen - mbt->strides[depth],
		   org_pfxlen,
		   old,
		   new);
	//Verify that our sub-vector remains in place
	smp_fence(StoreLoad);
	if (!check_remains(slotp, cur))
	{
	    //Sub-vector does not remain, redo our updates
	    continue;//Retry
	}
	//Sub-vector remains in place, we are done
	break;//Success
    }
    atomic_ptr_release(&hp, mbt->use_hp);
}

static void
update_pfx(p64_mbtrie_t *mbt,
	   uint32_t depth,
	   void **vec,
	   uint64_t pfx,
	   uint32_t pfxlen,
	   const uint32_t org_pfxlen,
	   p64_mbtrie_elem_t *old,
	   p64_mbtrie_elem_t *new)
{
    assert(depth < mbt->nstrides);
    uint32_t stride = mbt->strides[depth];
    if (UNLIKELY(stride == 0))
    {
	//No more levels
	report_error("mbtrie", "internal error at "__FILE__":"STRSTR(__LINE__),
		     org_pfxlen);
	return;
    }
    if (pfxlen <= stride)
    {
	//Prefix covers one or more slots in vector
	//Iterate over slots
	size_t nslots = stride_to_nslots(stride - pfxlen);
	size_t idx = prefix_to_index(pfx, stride);
	for (size_t i = 0; i < nslots; i++)
	{
	    update_slot(mbt,
			depth + 1,
			&vec[idx + i],
			pfx << stride,
			0,
			org_pfxlen,
			old,
			new);
	}
    }
    else//pfxlen > stride
    {
	assert(depth + 1 < mbt->nstrides);
	assert(mbt->strides[depth + 1] != 0);
	size_t idx = prefix_to_index(pfx, stride);
	update_vec(mbt, depth, &vec[idx], pfx, pfxlen, org_pfxlen, old, new);
	if (new == NULL || GET_PFXLEN(new) < org_pfxlen)
	{
	    //We removed or replaced elements in sub-vector
	    //Check if sub-vector can be collapsed
	    collapse_vec(mbt, depth, &vec[idx]);
	}
    }
}

static void
replace_default(p64_mbtrie_t *mbt,
		p64_mbtrie_elem_t *new)
{
    assert(!HAS_ANY(new));
    void **slotp = (void **)&mbt->default_pfx;
    //Insert new default prefix ("default gateway")
    p64_hazardptr_t hp = P64_HAZARDPTR_NULL;
    for (;;)
    {
	void *cur = atomic_load_acquire(slotp, &hp, ~ALL_BITS, mbt->use_hp);
	if (swing_slot(mbt, slotp, cur, new))
	{
	    break;//Success
	}
    }
    atomic_ptr_release(&hp, mbt->use_hp);
}

void
p64_mbtrie_insert(p64_mbtrie_t *mbt,
		  uint64_t pfx,
		  uint32_t pfxlen,
                  p64_mbtrie_elem_t *elem)
{
    p64_mbtrie_remove(mbt, pfx, pfxlen, NULL, elem);
}

//insert: old = NULL, new = element to insert
//remove: old = element to remove, new = replacement element or NULL
void
p64_mbtrie_remove(p64_mbtrie_t *mbt,
		  uint64_t pfx,
		  uint32_t pfxlen,
		  p64_mbtrie_elem_t *old,
		  p64_mbtrie_elem_t *new)
{
    if (UNLIKELY(HAS_ANY(old)))
    {
	report_error("mbtrie", "element has low bits set", old);
	return;
    }
    if (UNLIKELY(HAS_ANY(new)))
    {
	report_error("mbtrie", "element has low bits set", new);
	return;
    }
    if (old == NULL)
    {
	//Insert 'new'
	if (UNLIKELY(new == NULL))
	{
	    report_error("mbtrie", "null element", 0);
	    return;
	}
    }
    //Remove 'old', replace with 'new' (may be NULL)
    if (UNLIKELY(pfxlen > mbt->maxlen))
    {
	report_error("mbtrie", "prefix too long", pfxlen);
	return;
    }
    if (UNLIKELY((pfx & ~pfxlen_to_mask(pfxlen)) != 0))
    {
	report_error("mbtrie", "prefix has unused bits set", pfx);
	return;
    }
    if (!mbt->use_hp)
    {
	p64_qsbr_acquire();
    }
    if (UNLIKELY(pfxlen == 0))
    {
	//Insert/replace new default prefix ("default gateway")
	replace_default(mbt, new);
    }
    else
    {
	if (new == mbt->default_pfx)
	{
	    //We use NULL to represent the default prefix
	    new = NULL;
	}
	else
	{
	    //All non-NULL elements must to have the prefix length embedded
	    new = SET_PFXLEN(new, pfxlen);
	}
	//Increment reference conuters to ensure elements don't get reclaimed
	//during update
	increment_refcnt(old, 1);
	increment_refcnt(new, 1);
	//Update the multi-bit trie
	update_pfx(mbt, 0, mbt->base, pfx, pfxlen, pfxlen, old, new);
	//Corresponding decrement of reference counters
	decrement_refcnt(mbt, new, 1);
	decrement_refcnt(mbt, old, 1);
    }
    if (!mbt->use_hp)
    {
	p64_qsbr_release();
    }
}

static inline p64_mbtrie_elem_t *
lookup(p64_mbtrie_t *mbt,
       uint64_t key,
       p64_hazardptr_t *hp,
       bool use_hp)
{
    const uint64_t org_key = key;
    p64_hazardptr_t hpprev = P64_HAZARDPTR_NULL;
    const uint8_t *strides = mbt->strides;
    void **vec = mbt->base;
    //Loop will terminate if trie is built correctly
    do
    {
	size_t idx = prefix_to_index(key, *strides);
	void *ptr = atomic_load_acquire(&vec[idx], hp, ~ALL_BITS, use_hp);
	if (!IS_VECTOR(ptr))
	{
	    ptr = CLR_ALL(ptr);
	    if (ptr == NULL)
	    {
		//NULL means default prefix
		ptr = atomic_load_acquire((void **)&mbt->default_pfx,
					  hp,
					  ~ALL_BITS,
					  use_hp);
	    }
	    if (UNLIKELY(use_hp))
	    {
		p64_hazptr_release_ro(&hpprev);
	    }
	    //Caller must release hp
	    return ptr;
	}
	vec = CLR_ALL(ptr);
	assert(vec != NULL);
	//Prepare for next iteration
	if (use_hp)
	{
	    SWAP(hpprev, *hp);
	}
	key <<= *strides++;
    }
    while (*strides != 0);
    report_error("mbtrie", "internal error at "__FILE__":"STRSTR(__LINE__),
		 org_key);
    return NULL;
}

p64_mbtrie_elem_t *
p64_mbtrie_lookup(p64_mbtrie_t *mbt,
                  uint64_t key,
                  p64_hazardptr_t *hp)
{
    p64_mbtrie_elem_t *ptr;
    if (!mbt->use_hp)
    {
	ptr = lookup(mbt, key, NULL, false);
    }
    else
    {
	ptr = lookup(mbt, key, hp, true);
    }
    return ptr;
}

static unsigned long
parse_data(unsigned long mask,
	   p64_mbtrie_t *mbt,
	   uint32_t depth,
	   void **ptrs[],
	   uint64_t keys[],
	   unsigned long *success)
{
    unsigned long next_mask = 0;
    do
    {
	size_t i = __builtin_ctzl(mask);
	unsigned long bit = 1UL << i;
	mask &= ~bit;
	void *ptr = ptrs[i];
	if (IS_VECTOR(ptr))
	{
	    //Follow pointer to subvector
	    next_mask |= bit;
	    //Adjust key for next level
	    keys[i] <<= mbt->strides[depth];
	    size_t idx = prefix_to_index(keys[i], mbt->strides[depth + 1]);
	    void **vec = CLR_ALL(ptr);
	    //Follow pointer in trie table
	    ptrs[i] = (void **)__atomic_load_n(&vec[idx], __ATOMIC_ACQUIRE);
	}
	//Else leaf found, stop here (don't update next_mask)
	else if (LIKELY(ptr != NULL))
	{
	    //Non-null leaf
success:
	    ptr = CLR_ALL(ptr);
	    ptrs[i] = ptr;
	    //Assume user will dereference next-hop data soon
	    PREFETCH_FOR_READ(ptr);
	    *success |= bit;
	}
	else//ptr == NULL
	{
	    //NULL means default prefix (if any)
	    ptr = __atomic_load_n(&mbt->default_pfx, __ATOMIC_ACQUIRE);
	    if (LIKELY(ptr != NULL))
	    {
		goto success;
	    }
	}
    }
    while (LIKELY(mask != 0));
    return next_mask;
}

unsigned long
p64_mbtrie_lookup_vec(p64_mbtrie_t *mbt,
		      uint32_t num,
		      uint64_t keys[num],
		      p64_mbtrie_elem_t *results[num])
{
    uint32_t long_bits = sizeof(long) * CHAR_BIT;
    if (UNLIKELY(num > long_bits))
    {
	report_error("mbtrie", "invalid vector size", num);
	return 0;
    }
    if (UNLIKELY(mbt->use_hp))
    {
	report_error("mbtrie", "hazard pointers not supported", 0);
	return 0;
    }
    if (UNLIKELY(num == 0))
    {
	return 0;
    }
    //First read all pointers to first level in order to utilise
    //memory level parallelism and overlap cache misses
    uint32_t depth = 0;
    for (uint32_t i = 0; i < num; i++)
    {
	size_t idx = prefix_to_index(keys[i], mbt->strides[depth]);
	void **vec = (void *)mbt->base;
	results[i] = (void *)__atomic_load_n(&vec[idx], __ATOMIC_ACQUIRE);
    }
    unsigned long mask = num == long_bits ? ~0UL : (1UL << num) - 1;
    unsigned long bm = 0;//Success bitmap
    do
    {
	//Then parse returned data, potentially stalling for cache misses
	mask = parse_data(mask,
			  mbt,
			  depth,
			  (void ***)results,
			  keys,
			  &bm);
    }
    while (mask != 0);
    return bm;
}
