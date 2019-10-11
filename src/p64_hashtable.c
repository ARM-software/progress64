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
#include "build_config.h"

#include "common.h"
#include "lockfree.h"
#include "os_abstraction.h"
#include "err_hnd.h"

#define MARK_REMOVE 1UL
#define HAS_MARK(ptr) (((uintptr_t)(ptr) & MARK_REMOVE) != 0)
#define SET_MARK(ptr) (void *)((uintptr_t)(ptr) |  MARK_REMOVE)
#define REM_MARK(ptr) (void *)((uintptr_t)(ptr) & ~MARK_REMOVE)

//CACHE_LINE == 32, __SIZEOF_POINTER__ == 4 => BKT_SIZE == 4
//CACHE_LINE == 64, __SIZEOF_POINTER__ == 8 => BKT_SIZE == 4
#define BKT_SIZE (CACHE_LINE / (2 * __SIZEOF_POINTER__))

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
    uint32_t nbkts;
    uint32_t nused;//elements
    struct hash_bucket buckets[];
};

static uint32_t
list_check(p64_hashelem_t *prnt, uint64_t (*f)(p64_hashelem_t *))
{
    uint32_t num = 0;
    p64_hashelem_t *he;
    while ((he = REM_MARK(prnt->next)) != NULL)
    {
	printf(" <h=%"PRIxPTR",k=%"PRIu64">", prnt->hash, f(he));
	num++;
	prnt = he;
    }
    return num;
}

static uint32_t
bucket_check(uint32_t bix,
	     struct hash_bucket *bkt,
	     uint64_t (*f)(p64_hashelem_t *))
{
    uint32_t num = 0;
    for (uint32_t i = 0; i < BKT_SIZE; i++)
    {
	printf("%u.%u:", bix, i);
	num += list_check(&bkt->elems[i], f);
	printf("\n");
    }
    return num;
}

uint32_t
p64_hashtable_check(p64_hashtable_t *ht,
		    uint64_t (*f)(p64_hashelem_t *))
{
    uint32_t num = 0;
    for (uint32_t i = 0; i < ht->nbkts; i++)
    {
	num += bucket_check(i, &ht->buckets[i], f);
    }
    printf("Found %u elements (%u)\n", num, ht->nused);
    return num;
}

p64_hashtable_t *
p64_hashtable_alloc(uint32_t nelems)
{
    size_t nbkts = (nelems + BKT_SIZE - 1) / BKT_SIZE;
    size_t sz = sizeof(p64_hashtable_t) +
		sizeof(struct hash_bucket) * nbkts;
    p64_hashtable_t *ht = p64_malloc(sz, CACHE_LINE);
    if (ht != NULL)
    {
	memset(ht, 0, sz);
	ht->nbkts = nbkts;
	ht->nused = 0;
	//All buckets already cleared (NULL pointers)
    }
    return ht;
}

void
p64_hashtable_free(p64_hashtable_t *ht)
{
    if (ht != NULL)
    {
#ifndef NDEBUG
	if (ht->nused != 0)
	{
	    report_error("hashtable", "hash table not empty", ht);
	    //return; TODO?
	}
#endif
	p64_mfree(ht);
    }
}

UNROLL_LOOPS ALWAYS_INLINE
static inline p64_hashelem_t *
bucket_lookup(struct hash_bucket *bkt,
	      p64_hashtable_compare cf,
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
	p64_hashelem_t *he = p64_hazptr_acquire(&prnt->next, hazpp);
	//The head element pointers cannot be marked for REMOVAL
	assert(REM_MARK(he) == he);
	if (he != NULL)
	{
	    if (cf(he, key) == 0)
	    {
		//Found our element
		return he;
	    }
	}
	mask &= ~(1U << i);
    }
    return NULL;
}

static p64_hashelem_t *
list_lookup(p64_hashelem_t *prnt,
	    p64_hashtable_compare cf,
	    const void *key,
	    p64_hazardptr_t *hazpp)
{
    p64_hazardptr_t hpprnt = P64_HAZARDPTR_NULL;
    for (;;)
    {
	p64_hashelem_t *this = p64_hazptr_acquire(&prnt->next, hazpp);
	this = REM_MARK(this);
	if (this == NULL)
	{
	    p64_hazptr_release_ro(&hpprnt);
	    return NULL;
	}
	if (cf(this, key) == 0)
	{
	    //Found our element
	    p64_hazptr_release_ro(&hpprnt);
	    return this;
	}
	//Continue search
	prnt = this;
	SWAP(hpprnt, *hazpp);
    }
}

p64_hashelem_t *
p64_hashtable_lookup(p64_hashtable_t *ht,
		     p64_hashtable_compare cf,
		     const void *key,
		     p64_hashvalue_t hash,
		     p64_hazardptr_t *hazpp)
{
    uint32_t bix = (hash / BKT_SIZE) % ht->nbkts;
    struct hash_bucket *bkt = &ht->buckets[bix];
    p64_hashelem_t *he;
    *hazpp = P64_HAZARDPTR_NULL;
    he = bucket_lookup(bkt, cf, key, hash, hazpp);
    if (he != NULL)
    {
	return he;
    }
    he = list_lookup(&bkt->elems[hash % BKT_SIZE], cf, key, hazpp);
    if (he != NULL)
    {
	return he;
    }
    p64_hazptr_release_ro(hazpp);
    return NULL;
}

//Remove node, return true if element removed by us or some other thread
//Returns fails if element cannot be removed due to parent marked for removal
static inline bool
remove_node(p64_hashelem_t *prnt,
	    p64_hashelem_t *this,
	    p64_hashvalue_t hash,
	    int32_t *removed)
{
    assert(this == REM_MARK(this));
    //Set our REMOVE mark (it may already be set)
    __atomic_fetch_or((uintptr_t *)&this->next, MARK_REMOVE, __ATOMIC_RELAXED);
    //Now nobody may update our next pointer
    //And other threads may help to remove us
    //Swing our parent's next pointer

    //Expect prnt->this to be unmarked or parent is also marked for removal
    union heui old = {.he.next = this, .he.hash = hash };
    //New prnt->next should not have REMOVAL mark
    union heui neu = {.he.next = REM_MARK(this->next), .he.hash = this->hash };
    if (lockfree_compare_exchange_pp((ptrpair_t *)prnt,
				       &old.pp,
				       neu.pp,
				       /*weak=*/false,
				       __ATOMIC_RELAXED,
				       __ATOMIC_RELAXED))
    {
	(*removed)++;
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
    if (lockfree_compare_exchange_pp((ptrpair_t *)prnt,
				       &old.pp,
				       neu.pp,
				       /*weak=*/false,
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
	mask &= ~(1U << i);
    }
    return false;
}

static void
list_insert(p64_hashelem_t *prnt,
	    p64_hashelem_t *he,
	    p64_hashvalue_t hash,
	    int32_t *removed)
{
    p64_hazardptr_t hpprnt = P64_HAZARDPTR_NULL;
    p64_hazardptr_t hpthis = P64_HAZARDPTR_NULL;
    p64_hashelem_t *const org = prnt;
    for (;;)
    {
	p64_hashelem_t *this = p64_hazptr_acquire(&prnt->next, &hpthis);
	this = REM_MARK(this);
	if (this == NULL)
	{
	    //Next pointer is NULL => end of list, try to swap in our element
	    p64_hashelem_t *old = insert_node(prnt, he, hash);
	    if (old == NULL)
	    {
		//CAS succeeded, our element added to end of list
		p64_hazptr_release(&hpprnt);
		p64_hazptr_release_ro(&hpthis);
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
	    p64_hazptr_release(&hpprnt);
	    p64_hazptr_release_ro(&hpthis);
	    return;
	}
	else if (UNLIKELY(HAS_MARK(this->next)))
	{
	    //Found other element ('this' != 'he') marked for removal
	    //Let's give a helping hand
	    //FIXME prnt->hash not read atomically with prnt->next, problem?
	    if (remove_node(prnt, this, prnt->hash, removed))
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

void
p64_hashtable_insert(p64_hashtable_t *ht,
		     p64_hashelem_t *he,
		     p64_hashvalue_t hash)
{
    int32_t removed = -1;//Assume one node inserted
    uint32_t bix = (hash / BKT_SIZE) % ht->nbkts;
    struct hash_bucket *bkt = &ht->buckets[bix];
    he->hash = 0;
    he->next = NULL;
    bool success = bucket_insert(bkt, he, hash);
    if (!success)
    {
	p64_hashelem_t *prnt = &bkt->elems[hash % BKT_SIZE];
	list_insert(prnt, he, hash, &removed);
    }
#ifndef NDEBUG
    if (removed != 0)
    {
	__atomic_fetch_sub(&ht->nused, removed, __ATOMIC_RELAXED);
    }
#endif
}

UNROLL_LOOPS ALWAYS_INLINE
static inline bool
bucket_remove(struct hash_bucket *bkt,
	      p64_hashelem_t *he,
	      p64_hashvalue_t hash,
	      int32_t *removed)
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
	//No need to p64_hazptr_acquire(), we already have a reference
	//Cannot fail due to parent marked for removal
	(void)remove_node(prnt, he, hash, removed);
	return true;
    }
    return false;
}

static bool
list_remove(p64_hashelem_t *prnt,
	    p64_hashelem_t *he,
	    p64_hashvalue_t hash,
	    int32_t *removed)
{
    p64_hazardptr_t hpprnt = P64_HAZARDPTR_NULL;
    p64_hazardptr_t hpthis = P64_HAZARDPTR_NULL;
    p64_hazardptr_t hpnext = P64_HAZARDPTR_NULL;
    p64_hashelem_t *const org = prnt;
    for (;;)
    {
	p64_hashelem_t *this = p64_hazptr_acquire(&prnt->next, &hpthis);
	this = REM_MARK(this);
	if (UNLIKELY(this == NULL))
	{
	    //End of list
	    p64_hazptr_release(&hpprnt);
	    p64_hazptr_release(&hpthis);
	    p64_hazptr_release(&hpnext);
	    return false;//Element not found
	}
	else if (this == he)
	{
	    //Found our element, now remove it
	    if (remove_node(prnt, this, hash, removed))
	    {
		//Success, 'this' node is removed
		p64_hazptr_release(&hpprnt);
		p64_hazptr_release(&hpthis);
		p64_hazptr_release(&hpnext);
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
	    if (remove_node(prnt, this, prnt->hash, removed))
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
    int32_t removed = 0;
    uint32_t bix = (hash / BKT_SIZE) % ht->nbkts;
    struct hash_bucket *bkt = &ht->buckets[bix];
    bool success = bucket_remove(bkt, he, hash, &removed);
    if (!success)
    {
	p64_hashelem_t *prnt = &bkt->elems[hash % BKT_SIZE];
	success = list_remove(prnt, he, hash, &removed);
    }
#ifndef NDEBUG
    if (removed != 0)
    {
	__atomic_fetch_sub(&ht->nused, removed, __ATOMIC_RELAXED);
    }
#endif
    return success;
}

UNROLL_LOOPS ALWAYS_INLINE
static inline p64_hashelem_t *
bucket_remove_by_key(struct hash_bucket *bkt,
		     p64_hashtable_compare cf,
		     const void *key,
		     p64_hashvalue_t hash,
		     p64_hazardptr_t *hazpp,
		     int32_t *removed)
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
	p64_hashelem_t *he = p64_hazptr_acquire(&prnt->next, hazpp);
	//The head element pointers cannot be marked for REMOVAL
	assert(REM_MARK(he) == he);
	if (he != NULL)
	{
	    if (cf(he, key) == 0)
	    {
		//Found our element
		//Cannot fail due to parent marked for removal
		(void)remove_node(prnt, he, hash, removed);
		return he;
	    }
	}
	mask &= ~(1U << i);
    }
    return NULL;
}

static p64_hashelem_t *
list_remove_by_key(p64_hashelem_t *prnt,
		   p64_hashtable_compare cf,
		   const void *key,
		   p64_hashvalue_t hash,
		   p64_hazardptr_t *hazpp,
		   int32_t *removed)
{
    p64_hazardptr_t hpprnt = P64_HAZARDPTR_NULL;
    p64_hazardptr_t hpthis = P64_HAZARDPTR_NULL;
    p64_hazardptr_t hpnext = P64_HAZARDPTR_NULL;
    p64_hashelem_t *const org = prnt;
    for (;;)
    {
	p64_hashelem_t *this = p64_hazptr_acquire(&prnt->next, &hpthis);
	this = REM_MARK(this);
	if (UNLIKELY(this == NULL))
	{
	    //End of list
	    p64_hazptr_release(&hpprnt);
	    p64_hazptr_release(&hpthis);
	    p64_hazptr_release(&hpnext);
	    return NULL;//Element not found
	}
	else if (cf(this, key) == 0)
	{
	    //Found our element, now remove it
	    if (remove_node(prnt, this, hash, removed))
	    {
		//Success, 'this' node is removed
		p64_hazptr_release(&hpprnt);
		p64_hazptr_release(&hpnext);
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
	    if (remove_node(prnt, this, prnt->hash, removed))
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
			    p64_hashtable_compare cf,
			    const void *key,
			    p64_hashvalue_t hash,
			    p64_hazardptr_t *hazpp)
{
    int32_t removed = 0;
    uint32_t bix = (hash / BKT_SIZE) % ht->nbkts;
    struct hash_bucket *bkt = &ht->buckets[bix];
    p64_hashelem_t *he = bucket_remove_by_key(bkt, cf, key, hash,
					      hazpp, &removed);
    if (he == NULL)
    {
	p64_hashelem_t *prnt = &bkt->elems[hash % BKT_SIZE];
	he = list_remove_by_key(prnt, cf, key, hash, hazpp, &removed);
    }
#ifndef NDEBUG
    if (removed != 0)
    {
	__atomic_fetch_sub(&ht->nused, removed, __ATOMIC_RELAXED);
    }
#endif
    return he;
}
