//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <assert.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "p64_hashtable.h"
#undef p64_hashtable_lookup
#undef p64_hashtable_remove_by_key
#include "p64_hazardptr.h"
#include "p64_qsbr.h"
#include "build_config.h"

#include "common.h"
#include "atomic.h"
#include "os_abstraction.h"
#include "err_hnd.h"

#define MARK_REMOVE (uintptr_t)1
#define HAS_MARK(ptr) (((uintptr_t)(ptr) & MARK_REMOVE) != 0)
#define SET_MARK(ptr) (void *)((uintptr_t)(ptr) |  MARK_REMOVE)
#define REM_MARK(ptr) (void *)((uintptr_t)(ptr) & ~MARK_REMOVE)

//CACHE_LINE == 32, __SIZEOF_POINTER__ == 4 => BKT_SIZE == 4
//CACHE_LINE == 64, __SIZEOF_POINTER__ == 8 => BKT_SIZE == 4
#define BKT_SIZE (CACHE_LINE / (2 * __SIZEOF_POINTER__))

static inline void *
atomic_load_acquire(struct p64_hashelem **pptr,
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
	return atomic_load_ptr(pptr, __ATOMIC_ACQUIRE);
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

struct hash_bucket
{
    p64_hashelem_t elems[BKT_SIZE];
} ALIGNED(CACHE_LINE);

union heui
{
    p64_hashelem_t he;
    ptrpair_t pp;
};

struct p64_hashtable
{
    p64_hashtable_compare cf;
    size_t nbkts;
    uint8_t use_hp;
    struct hash_bucket buckets[] ALIGNED(CACHE_LINE);
};

static inline size_t
hash_to_bix(p64_hashtable_t *ht, p64_hashvalue_t hash)
{
    return (hash / BKT_SIZE) % ht->nbkts;
}

static void
traverse_list(p64_hashelem_t *prnt,
	      p64_hashtable_trav_cb cb,
	      void *arg,
	      size_t idx,
	      bool use_hp)
{
    p64_hazardptr_t hpprnt = P64_HAZARDPTR_NULL;
    p64_hazardptr_t hpthis = P64_HAZARDPTR_NULL;
    if (!use_hp)
    {
	p64_qsbr_acquire();
    }
    for (;;)
    {
	p64_hashelem_t *this = atomic_load_acquire(&prnt->next,
						   &hpthis,
						   ~MARK_REMOVE,
						   use_hp);
	this = REM_MARK(this);
	if (this == NULL)
	{
	    break;
	}
	cb(arg, this, idx);
	prnt = this;
	SWAP(hpprnt, hpthis);
    }
    if (!use_hp)
    {
	p64_qsbr_release();
    }
    atomic_ptr_release(&hpprnt, use_hp);
    atomic_ptr_release(&hpthis, use_hp);
}

static void
traverse_bucket(p64_hashtable_t *ht,
		size_t bix,
		p64_hashtable_trav_cb cb,
		void *arg)
{
    struct hash_bucket *bkt = &ht->buckets[bix];
    for (uint32_t i = 0; i < BKT_SIZE; i++)
    {
	traverse_list(&bkt->elems[i], cb, arg, bix * BKT_SIZE + i, ht->use_hp);
    }
}

void
p64_hashtable_traverse(p64_hashtable_t *ht,
		       p64_hashtable_trav_cb cb,
		       void *arg)
{
    for (size_t i = 0; i < ht->nbkts; i++)
    {
	traverse_bucket(ht, i, cb, arg);
    }
}

#define VALID_FLAGS P64_HASHTAB_F_HP

p64_hashtable_t *
p64_hashtable_alloc(size_t nelems,
		    p64_hashtable_compare cf,
		    uint32_t flags)
{
    if (UNLIKELY(nelems == 0))
    {
	report_error("hashtable", "invalid number of elements", nelems);
	return NULL;
    }
    if (UNLIKELY((flags & ~VALID_FLAGS) != 0))
    {
        report_error("hashtable", "invalid flags", flags);
        return NULL;
    }
    size_t nbkts = (nelems + BKT_SIZE - 1) / BKT_SIZE;
    size_t sz = sizeof(p64_hashtable_t) +
		sizeof(struct hash_bucket) * nbkts;
    p64_hashtable_t *ht = p64_malloc(sz, CACHE_LINE);
    if (ht != NULL)
    {
	memset(ht, 0, sz);
	ht->cf = cf;
	ht->nbkts = nbkts;
	ht->use_hp = (flags & P64_HASHTAB_F_HP) != 0;
	//All buckets already cleared (NULL pointers)
    }
    return ht;
}

#undef VALID_FLAGS

void
p64_hashtable_free(p64_hashtable_t *ht)
{
    if (ht != NULL)
    {
	//Check if hash table is empty
	for (size_t i = 0; i < ht->nbkts; i++)
	{
	    for (uint32_t j = 0; j < BKT_SIZE; j++)
	    {
		//No need to use HP or QSBR, elements are not accessed
		if (ht->buckets[i].elems[j].next != NULL)
		{
		    report_error("hashtable", "hash table not empty", 0);
		    return;
		}
	    }
	}
	p64_mfree(ht);
    }
}

UNROLL_LOOPS ALWAYS_INLINE
static inline p64_hashelem_t *
bucket_lookup(p64_hashtable_t *ht,
	      struct hash_bucket *bkt,
	      const void *key,
	      p64_hashvalue_t hash,
	      p64_hazardptr_t *hazpp,
	      bool check_key)
{
    uint32_t mask = 0;
    //We want this loop unrolled
    for (uint32_t i = 0; i < BKT_SIZE; i++)
    {
	if (bkt->elems[i].hash == hash)
	{
	    mask |= 1U << i;
	}
    }
    while (mask != 0)
    {
	uint32_t i = __builtin_ctz(mask);
	p64_hashelem_t *prnt = &bkt->elems[i];
	p64_hashelem_t *he = atomic_load_acquire(&prnt->next,
						 hazpp,
						 ~MARK_REMOVE,
						 ht->use_hp);
	//The head element pointers cannot be marked for REMOVAL
	assert(REM_MARK(he) == he);
	if (he != NULL)
	{
	    //Already matched on hash above
	    if (check_key)
	    {
		if (ht->cf(he, key) == 0)
		{
		    //Found our element
		    return he;
		}
		//Else false positive
	    }
	    else//Check key later
	    {
		PREFETCH_FOR_READ(he);
		return he;
	    }
	}
	//Clear least significant bit
	mask &= mask - 1;
    }
    return NULL;
}

static p64_hashelem_t *
list_lookup(p64_hashtable_t *ht,
	    p64_hashelem_t *prnt,
	    const void *key,
	    p64_hazardptr_t *hazpp,
	    bool check_key)
{
    p64_hazardptr_t hpprnt = P64_HAZARDPTR_NULL;
    for (;;)
    {
	p64_hashelem_t *this = atomic_load_acquire(&prnt->next,
						   hazpp,
						   ~MARK_REMOVE,
						   ht->use_hp);
	this = REM_MARK(this);
	if (this == NULL)
	{
	    atomic_ptr_release(&hpprnt, ht->use_hp);
	    return NULL;
	}
	if (check_key)
	{
	    if (ht->cf(this, key) == 0)
	    {
		//Found our element
		atomic_ptr_release(&hpprnt, ht->use_hp);
		return this;
	    }
	    //Else false positive
	}
	else//Check key later
	{
	    PREFETCH_FOR_READ(this);
	    atomic_ptr_release(&hpprnt, ht->use_hp);
	    return this;
	}
	//Continue search
	prnt = this;
	SWAP(hpprnt, *hazpp);
    }
}

ALWAYS_INLINE
static inline p64_hashelem_t *
lookup(p64_hashtable_t *ht,
       struct hash_bucket *bkt,
       const void *key,
       p64_hashvalue_t hash,
       p64_hazardptr_t *hazpp,
       bool check_key)
{
    p64_hashelem_t *he;
    he = bucket_lookup(ht, bkt, key, hash, hazpp, check_key);
    if (he != NULL)
    {
	return he;
    }
    he = list_lookup(ht, &bkt->elems[hash % BKT_SIZE], key, hazpp, check_key);
    if (he != NULL)
    {
	return he;
    }
    return NULL;
}

p64_hashelem_t *
p64_hashtable_lookup(p64_hashtable_t *ht,
		     const void *key,
		     p64_hashvalue_t hash,
		     p64_hazardptr_t *hazpp)
{
    //Caller must call QSBR acquire/release/quiescent as appropriate
    p64_hazardptr_t hp = P64_HAZARDPTR_NULL;
    if (hazpp == NULL)
    {
	hazpp = &hp;
    }
    size_t bix = hash_to_bix(ht, hash);
    struct hash_bucket *bkt = &ht->buckets[bix];
    p64_hashelem_t *he = lookup(ht, bkt, key, hash, hazpp, true);
    return he;
}

void
p64_hashtable_lookup_vec(p64_hashtable_t *ht,
			 uint32_t num,
			 const void *keys[num],
			 p64_hashvalue_t hashes[num],
			 p64_hashelem_t *result[num])
{
    //Caller must call QSBR acquire/release/quiescent as appropriate
    if (UNLIKELY(ht->use_hp))
    {
	report_error("hashtable", "hazard pointers not supported", 0);
    }
    struct hash_bucket *bkts[num];
    for (uint32_t i = 0; i < num; i++)
    {
	size_t bix = hash_to_bix(ht, hashes[i]);
	bkts[i] = &ht->buckets[bix];
	PREFETCH_FOR_READ(bkts[i]);
    }
    for (uint32_t i = 0; i < num; i++)
    {
	p64_hazardptr_t hp = P64_HAZARDPTR_NULL;
	result[i] = lookup(ht, bkts[i], keys[i], hashes[i], &hp, false);
    }
    for (uint32_t i = 0; i < num; i++)
    {
	if (result[i] != NULL)
	{
	    if (ht->cf(result[i], keys[i]) != 0)
	    {
		p64_hazardptr_t hp = P64_HAZARDPTR_NULL;
		result[i] = p64_hashtable_lookup(ht, keys[i], hashes[i], &hp);
	    }
	}
    }
}

//Remove node, return true if element removed by us or some other thread
//Returns fails if element cannot be removed due to parent marked for removal
static inline bool
remove_node(p64_hashelem_t *prnt,
	    p64_hashelem_t *this,
	    p64_hashvalue_t hash)
{
    assert(this == REM_MARK(this));
    //Set our REMOVE mark (it may already be set)
    atomic_fetch_or(&this->next, MARK_REMOVE, __ATOMIC_RELAXED);
    //Now nobody may update our next pointer
    //And other threads may help to remove us
    //Swing our parent's next pointer

    //Expect prnt->this to be unmarked or parent is also marked for removal
    union heui old = {.he.next = this, .he.hash = hash };
    //New prnt->next should not have REMOVAL mark
    union heui neu = {.he.next = REM_MARK(this->next), .he.hash = this->hash };
    if (atomic_compare_exchange_n((ptrpair_t *)prnt,
				  &old.pp,
				  neu.pp,
				  __ATOMIC_RELAXED,
				  __ATOMIC_RELAXED))
    {
	return true;
    }
    else if (REM_MARK(old.he.next) != this)
    {
	//prnt->next doesn't point to 'this', 'this' already removed
	return true;
    }
    //Else prnt->next does point to 'this' but parent marked for removal
    assert(old.he.next == SET_MARK(this));
    return false;
}

static inline p64_hashelem_t *
insert_node(p64_hashelem_t *prnt,
	    p64_hashelem_t *he,
	    p64_hashvalue_t hash)
{
    assert(he->hash == 0);
    assert(he->next == NULL);
    union heui old = {.he.next = NULL, .he.hash = 0 };
    union heui neu = {.he.next = he, .he.hash = hash };
    if (atomic_compare_exchange_n((ptrpair_t *)prnt,
				  &old.pp,
				  neu.pp,
				  __ATOMIC_RELEASE,
				  __ATOMIC_RELAXED))
    {
	//CAS succeded, node inserted
	return NULL;
    }
    //CAS failed, unexpected value returned
    return old.he.next;
}

UNROLL_LOOPS ALWAYS_INLINE
static inline bool
bucket_insert(struct hash_bucket *bkt,
	      p64_hashelem_t *he,
	      p64_hashvalue_t hash)
{
    uint32_t mask = 0;
    //We want this loop unrolled
    for (uint32_t i = 0; i < BKT_SIZE; i++)
    {
	if (bkt->elems[i].next == NULL)
	{
	    mask |= 1U << i;
	}
    }
    while (mask != 0)
    {
	uint32_t i = __builtin_ctz(mask);
	if (insert_node(&bkt->elems[i], he, hash) == NULL)
	{
	    //Success
	    return true;
	}
	//Clear least significant bit
	mask &= mask - 1;
    }
    return false;
}

static void
list_insert(p64_hashtable_t *ht,
	    p64_hashelem_t *prnt,
	    p64_hashelem_t *he,
	    p64_hashvalue_t hash)
{
    p64_hazardptr_t hpprnt = P64_HAZARDPTR_NULL;
    p64_hazardptr_t hpthis = P64_HAZARDPTR_NULL;
    p64_hashelem_t *const org = prnt;
    for (;;)
    {
	p64_hashelem_t *this = atomic_load_acquire(&prnt->next,
						   &hpthis,
						   ~MARK_REMOVE,
						   ht->use_hp);
	this = REM_MARK(this);
	if (this == NULL)
	{
	    //Next pointer is NULL => end of list, try to swap in our element
	    p64_hashelem_t *old = insert_node(prnt, he, hash);
	    if (old == NULL)
	    {
		//CAS succeeded, our element added to end of list
		atomic_ptr_release(&hpprnt, ht->use_hp);
		atomic_ptr_release(&hpthis, ht->use_hp);
		return;//Element inserted
	    }
	    //Else CAS failed, next pointer unexpectedly changed
	    if (HAS_MARK(old))
	    {
		//Parent marked for removal and must be removed before we
		//remove 'this'
		//Restart from beginning
		prnt = org;
		continue;
	    }
	    //Other node inserted at this place, try again
	    continue;
	}
	else if (UNLIKELY(this == he))
	{
	    report_error("hashtable", "element already present", he);
	    atomic_ptr_release(&hpprnt, ht->use_hp);
	    atomic_ptr_release(&hpthis, ht->use_hp);
	    return;
	}
	else if (UNLIKELY(HAS_MARK(this->next)))
	{
	    //Found other element ('this' != 'he') marked for removal
	    //Let's give a helping hand
	    //FIXME prnt->hash not read atomically with prnt->next, problem?
	    if (remove_node(prnt, this, prnt->hash))
	    {
		//'this' node removed, '*prnt' points to 'next'
		//Continue from current position
		continue;
	    }
	    //Else parent node is also marked for removal
	    //Parent must be removed before we remove 'this'
	    //Restart from beginning
	    prnt = org;
	    continue;
	}
	//Continue search
	prnt = this;
	SWAP(hpprnt, hpthis);
    }
}

#define HAS_ANY(ptr) (((uintptr_t)(ptr) & MARK_REMOVE) != 0)

void
p64_hashtable_insert(p64_hashtable_t *ht,
		     p64_hashelem_t *he,
		     p64_hashvalue_t hash)
{
    if (UNLIKELY(HAS_ANY(he)))
    {
	report_error("hashtable", "element has low bits set", he);
	return;
    }
    if (!ht->use_hp)
    {
	p64_qsbr_acquire();
    }
    size_t bix = hash_to_bix(ht, hash);
    struct hash_bucket *bkt = &ht->buckets[bix];
    he->hash = 0;
    he->next = NULL;
    bool success = bucket_insert(bkt, he, hash);
    if (!success)
    {
	p64_hashelem_t *prnt = &bkt->elems[hash % BKT_SIZE];
	list_insert(ht, prnt, he, hash);
    }
    if (!ht->use_hp)
    {
	p64_qsbr_release();
    }
}

UNROLL_LOOPS ALWAYS_INLINE
static inline bool
bucket_remove(struct hash_bucket *bkt,
	      p64_hashelem_t *he,
	      p64_hashvalue_t hash)
{
    uint32_t mask = 0;
    //We want this loop unrolled
    for (uint32_t i = 0; i < BKT_SIZE; i++)
    {
	if (bkt->elems[i].next == he)
	{
	    mask |= 1U << i;
	}
    }
    if (mask != 0)
    {
	uint32_t i = __builtin_ctz(mask);
	p64_hashelem_t *prnt = &bkt->elems[i];
	//No need to atomic_load_acquire(), we already have a reference
	//Cannot fail due to parent marked for removal
	(void)remove_node(prnt, he, hash);
	return true;
    }
    return false;
}

static bool
list_remove(p64_hashtable_t *ht,
	    p64_hashelem_t *prnt,
	    p64_hashelem_t *he,
	    p64_hashvalue_t hash)
{
    p64_hazardptr_t hpprnt = P64_HAZARDPTR_NULL;
    p64_hazardptr_t hpthis = P64_HAZARDPTR_NULL;
    p64_hazardptr_t hpnext = P64_HAZARDPTR_NULL;
    p64_hashelem_t *const org = prnt;
    for (;;)
    {
	p64_hashelem_t *this = atomic_load_acquire(&prnt->next,
						   &hpthis,
						   ~MARK_REMOVE,
						   ht->use_hp);
	this = REM_MARK(this);
	if (UNLIKELY(this == NULL))
	{
	    //End of list
	    atomic_ptr_release(&hpprnt, ht->use_hp);
	    atomic_ptr_release(&hpthis, ht->use_hp);
	    atomic_ptr_release(&hpnext, ht->use_hp);
	    return false;//Element not found
	}
	else if (this == he)
	{
	    //Found our element, now remove it
	    if (remove_node(prnt, this, hash))
	    {
		//Success, 'this' node is removed
		atomic_ptr_release(&hpprnt, ht->use_hp);
		atomic_ptr_release(&hpthis, ht->use_hp);
		atomic_ptr_release(&hpnext, ht->use_hp);
		return true;
	    }
	    //Else parent node is also marked for removal
	    //Parent must be removed before we remove 'this'
	    //Restart from beginning
	    prnt = org;
	    continue;
	}
	else if (UNLIKELY(HAS_MARK(this->next)))
	{
	    //Found other element ('this' != 'he') marked for removal
	    //Let's give a helping hand
	    //FIXME prnt->hash not read atomically with prnt->next, problem?
	    if (remove_node(prnt, this, prnt->hash))
	    {
		//'this' node removed, '*prnt' points to 'next'
		//Continue from current position
		continue;
	    }
	    //Else parent node is also marked for removal
	    //Parent must be removed before we remove 'this'
	    //Restart from beginning
	    prnt = org;
	    continue;
	}
	//Continue search
	prnt = this;
	SWAP(hpprnt, hpthis);
    }
}

bool
p64_hashtable_remove(p64_hashtable_t *ht,
		     p64_hashelem_t *he,
		     p64_hashvalue_t hash)
{
    if (!ht->use_hp)
    {
	p64_qsbr_acquire();
    }
    size_t bix = hash_to_bix(ht, hash);
    struct hash_bucket *bkt = &ht->buckets[bix];
    bool success = bucket_remove(bkt, he, hash);
    if (!success)
    {
	p64_hashelem_t *prnt = &bkt->elems[hash % BKT_SIZE];
	success = list_remove(ht, prnt, he, hash);
    }
    if (!ht->use_hp)
    {
	p64_qsbr_release();
    }
    return success;
}

UNROLL_LOOPS ALWAYS_INLINE
static inline p64_hashelem_t *
bucket_remove_by_key(p64_hashtable_t *ht,
		     struct hash_bucket *bkt,
		     const void *key,
		     p64_hashvalue_t hash,
		     p64_hazardptr_t *hazpp)
{
    uint32_t mask = 0;
    //We want this loop unrolled
    for (uint32_t i = 0; i < BKT_SIZE; i++)
    {
	if (bkt->elems[i].hash == hash)
	{
	    mask |= 1U << i;
	}
    }
    while (mask != 0)
    {
	uint32_t i = __builtin_ctz(mask);
	p64_hashelem_t *prnt = &bkt->elems[i];
	p64_hashelem_t *he = atomic_load_acquire(&prnt->next,
						 hazpp,
						 ~MARK_REMOVE,
						 ht->use_hp);
	//The head element pointers cannot be marked for REMOVAL
	assert(REM_MARK(he) == he);
	if (he != NULL)
	{
	    if (ht->cf(he, key) == 0)
	    {
		//Found our element
		//Cannot fail due to parent marked for removal
		(void)remove_node(prnt, he, hash);
		return he;
	    }
	}
	//Clear least significant bit
	mask &= mask - 1;
    }
    return NULL;
}

static p64_hashelem_t *
list_remove_by_key(p64_hashtable_t *ht,
		   p64_hashelem_t *prnt,
		   const void *key,
		   p64_hashvalue_t hash,
		   p64_hazardptr_t *hazpp)
{
    p64_hazardptr_t hpprnt = P64_HAZARDPTR_NULL;
    p64_hazardptr_t hpthis = P64_HAZARDPTR_NULL;
    p64_hazardptr_t hpnext = P64_HAZARDPTR_NULL;
    p64_hashelem_t *const org = prnt;
    for (;;)
    {
	p64_hashelem_t *this = atomic_load_acquire(&prnt->next,
						   &hpthis,
						   ~MARK_REMOVE,
						   ht->use_hp);
	this = REM_MARK(this);
	if (UNLIKELY(this == NULL))
	{
	    //End of list
	    atomic_ptr_release(&hpprnt, ht->use_hp);
	    atomic_ptr_release(&hpthis, ht->use_hp);
	    atomic_ptr_release(&hpnext, ht->use_hp);
	    return NULL;//Element not found
	}
	else if (ht->cf(this, key) == 0)
	{
	    //Found our element, now remove it
	    if (remove_node(prnt, this, hash))
	    {
		//Success, 'this' node is removed
		atomic_ptr_release(&hpprnt, ht->use_hp);
		atomic_ptr_release(&hpnext, ht->use_hp);
		*hazpp = hpthis;
		return this;
	    }
	    //Else parent node is also marked for removal
	    //Parent must be removed before we remove 'this'
	    //Restart from beginning
	    prnt = org;
	    continue;
	}
	else if (UNLIKELY(HAS_MARK(this->next)))
	{
	    //Found other element ('this' != 'he') marked for removal
	    //Let's give a helping hand
	    //FIXME prnt->hash not read atomically with prnt->next, problem?
	    if (remove_node(prnt, this, prnt->hash))
	    {
		//'this' node removed, '*prnt' points to 'next'
		//Continue from current position
		continue;
	    }
	    //Else parent node is also marked for removal
	    //Parent must be removed before we remove 'this'
	    //Restart from beginning
	    prnt = org;
	    continue;
	}
	//Continue search
	prnt = this;
	SWAP(hpprnt, hpthis);
    }
}

p64_hashelem_t *
p64_hashtable_remove_by_key(p64_hashtable_t *ht,
			    const void *key,
			    p64_hashvalue_t hash,
			    p64_hazardptr_t *hazpp)
{
    //Caller must call QSBR acquire/release/quiescent as appropriate
    size_t bix = hash_to_bix(ht, hash);
    struct hash_bucket *bkt = &ht->buckets[bix];
    p64_hashelem_t *he = bucket_remove_by_key(ht, bkt, key, hash, hazpp);
    if (he == NULL)
    {
	p64_hashelem_t *prnt = &bkt->elems[hash % BKT_SIZE];
	he = list_remove_by_key(ht, prnt, key, hash, hazpp);
    }
    return he;
}
