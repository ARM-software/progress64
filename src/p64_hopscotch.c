//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "p64_hopscotch.h"
#undef p64_hopscotch_lookup
#undef p64_hopscotch_remove_by_key
#include "p64_hazardptr.h"
#include "p64_qsbr.h"
#include "build_config.h"

#include "common.h"
#include "os_abstraction.h"
#include "err_hnd.h"
#include "atomic.h"

typedef size_t bix_t;

//CELLAR_BIT is used in traverse call-back to indicate index refers to cellar
#define CELLAR_BIT ((size_t)1 << (sizeof(size_t) * CHAR_BIT - 1))

static inline bix_t
ring_add(bix_t a, bix_t b, bix_t modulo)
{
    assert(a < modulo);
    assert(b < modulo);
    bix_t sum = a + b;
    if (UNLIKELY(sum >= modulo))
    {
	sum -= modulo;
    }
    assert(sum < modulo);
    return sum;
}

static inline bix_t
ring_sub(bix_t a, bix_t b, bix_t modulo)
{
    assert(a < modulo);
    assert(b < modulo);
    bix_t dif = a - b;
    if (UNLIKELY(dif >= modulo))
    {
	dif += modulo;
    }
    assert(dif < modulo);
    return dif;
}

static inline bix_t
ring_mod(p64_hopschash_t hash, bix_t modulo)
{
    bix_t rem = hash % modulo;
    assert(rem < modulo);
    return rem;
}

#if __SIZEOF_POINTER__ == 4
#define BITMAP_BITS 16
#define SIG_BITS 0 //No space for sig
#define COUNT_BITS 15
#else //__SIZEOF_POINTER__ == 8
#define BITMAP_BITS 24
#define SIG_BITS 8
#define COUNT_BITS 31
#endif

#define BITMAP_MASK (uint32_t)((1UL << BITMAP_BITS) - 1U)
#define SIG_MASK ((1U << SIG_BITS) - 1U)

union bmc
{
    struct
    {
	//Neighbourhood map of elements which hash to this bucket
	unsigned bitmap:BITMAP_BITS;
#if SIG_BITS != 0
	//Signature (truncated hash) of element stored in this bucket
	unsigned sig:SIG_BITS;
#endif
	//Change counter
	unsigned count:COUNT_BITS;
	//True if element(s) hashing to this bucket is stored in cellar
	unsigned cellar:1;
    };
    uintptr_t atom;//Need a scalar alias for use with atomic operations
};

static_assert(sizeof(union bmc) == sizeof(void *),
	     "sizeof(union bmc) == sizeof(void *)");

struct cell
{
    p64_hopschash_t hash;
    void *elem;
};

struct bucket
{
    union bmc bmc;
    void *elem;
};

struct p64_hopscotch
{
    p64_hopscotch_compare cf;
    bix_t nbkts;
    bix_t ncells;
    uint8_t use_hp;
    struct cell *cellar;//Pointer to cell array
    struct bucket buckets[] ALIGNED(CACHE_LINE);
    //Cell array follows the last bucket
};

static inline void *
atomic_load_acquire(void **pptr,
		    p64_hazardptr_t *hp,
		    bool use_hp)
{
    if (UNLIKELY(use_hp))
    {
	return p64_hazptr_acquire_mask(pptr, hp, ~0U);
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

void
p64_hopscotch_traverse(p64_hopscotch_t *ht,
		       p64_hopscotch_trav_cb cb,
		       void *arg)
{
    for (bix_t idx = 0; idx < ht->nbkts; idx++)
    {
	p64_hazardptr_t hp = P64_HAZARDPTR_NULL;
	void *elem = atomic_load_acquire(&ht->buckets[idx].elem,
					 &hp,
					 ht->use_hp);
	if (elem != NULL)
	{
	    if (!ht->use_hp)
	    {
		p64_qsbr_acquire();
		cb(arg, elem, idx);
		p64_qsbr_release();
	    }
	    else
	    {
		cb(arg, elem, idx);
	    }
	}
	atomic_ptr_release(&hp, ht->use_hp);
    }
    for (bix_t idx = 0; idx < ht->ncells; idx++)
    {
	p64_hazardptr_t hp = P64_HAZARDPTR_NULL;
	void *elem = atomic_load_acquire(&ht->cellar[idx].elem,
					 &hp,
					 ht->use_hp);
	if (elem != NULL)
	{
	    if (!ht->use_hp)
	    {
		p64_qsbr_acquire();
		cb(arg, elem, idx | CELLAR_BIT);
		p64_qsbr_release();
	    }
	    else
	    {
		cb(arg, elem, idx | CELLAR_BIT);
	    }
	}
	atomic_ptr_release(&hp, ht->use_hp);
    }
}

void
p64_hopscotch_check(p64_hopscotch_t *ht)
{
    size_t dist_hg[BITMAP_BITS] = { 0 };
    size_t full_hg[BITMAP_BITS + 1] = { 0 };
    size_t nfull = 0;
    size_t nelems = 0;
    uint32_t sliding_bm = 0;
    //Initialise sliding_bm from last BITMAP_BITS buckets
    for (bix_t bix = ht->nbkts - BITMAP_BITS; bix < ht->nbkts; bix++)
    {
	uint32_t bix_bm = ht->buckets[bix].bmc.bitmap;
	sliding_bm >>= 1;
	sliding_bm |= bix_bm;
    }
    for (bix_t bix = 0; bix < ht->nbkts; bix++)
    {
	uint32_t bix_bm = ht->buckets[bix].bmc.bitmap;
	sliding_bm >>= 1;
	if ((sliding_bm & bix_bm) != 0)
	{
	    fprintf(stderr, "%zu: (sliding_bm & bix_bm) != 0\n", bix);
	}
	sliding_bm |= bix_bm;
	assert((sliding_bm & ~BITMAP_MASK) == 0);
	if ((sliding_bm & 1) == 0)
	{
	    //Bucket should be empty
	    if (ht->buckets[bix].elem != NULL)
	    {
		fprintf(stderr, "%zu: ht->buckets[bix].elem not empty\n", bix);
	    }
	}
	else
	{
	    if (ht->buckets[bix].elem == NULL)
	    {
		fprintf(stderr, "%zu: ht->buckets[bix].elem empty\n", bix);
	    }
	}
	nelems += (ht->buckets[bix].elem != NULL);
	if (sliding_bm == BITMAP_MASK)
	{
	    nfull++;
	}
	full_hg[__builtin_popcount(bix_bm)]++;
	while (bix_bm != 0)
	{
	    uint32_t bit = __builtin_ctz(bix_bm);
	    //Clear least significant bit
	    bix_bm &= bix_bm - 1;
	    dist_hg[bit]++;
	}
    }
    size_t ncellar = 0;
    for (bix_t i = 0; i < ht->ncells; i++)
    {
	nelems += (ht->cellar[i].elem != NULL);
	ncellar += (ht->cellar[i].elem != NULL);
    }
    printf("Hopscotch hash table: %zu buckets, %zu cells, "
	    "%zu elements, load=%.2f\n",
	    ht->nbkts, ht->ncells,
	    nelems, (float)nelems / (ht->nbkts + ht->ncells));
    printf("Bitmap utilisation histogram:\n");
    for (uint32_t i = 0; i < BITMAP_BITS / 2; i++)
    {
	printf("%2u: %7zu (%.3f)\t\t%2u: %7zu (%.3f)\n",
		i, full_hg[i], full_hg[i] / (float)nelems,
		i + 1 + BITMAP_BITS / 2, full_hg[i + 1 + BITMAP_BITS / 2],
		full_hg[i + 1 + BITMAP_BITS / 2] / (float)nelems);
    }
    printf("%2u: %7zu (%.3f)\n",
	   BITMAP_BITS / 2,
	   full_hg[BITMAP_BITS / 2], full_hg[BITMAP_BITS / 2] / (float)nelems);
    printf("Cellar: %zu (%.3f)\n", ncellar, ncellar / (float)ht->ncells);
    printf("Distances from home bucket:\n");
    size_t sum = 0;
    for (uint32_t i = 0; i < BITMAP_BITS / 2; i++)
    {
	sum += i * dist_hg[i] +
	       (i + BITMAP_BITS / 2) * dist_hg[i + BITMAP_BITS / 2];
	printf("%2u: %7zu (%.3f)\t\t%2u: %7zu (%.3f)\n",
		i, dist_hg[i], dist_hg[i] / (float)nelems,
		i + BITMAP_BITS / 2, dist_hg[i + BITMAP_BITS / 2],
		dist_hg[i + BITMAP_BITS / 2] / (float)nelems);
    }
    printf("Avg distance: %.2f\n", sum / (float)nelems);
    printf("%zu (%.3f) neighbourhoods are completely full\n", nfull, nfull / (float)ht->nbkts);
}

#define VALID_FLAGS P64_HOPSCOTCH_F_HP

p64_hopscotch_t *
p64_hopscotch_alloc(size_t nbkts,
		    size_t ncells,
		    p64_hopscotch_compare cf,
		    uint32_t flags)
{
    if (UNLIKELY(nbkts < BITMAP_BITS))
    {
	report_error("hopscotch", "invalid number of elements", nbkts);
	return NULL;
    }
    if (UNLIKELY((flags & ~VALID_FLAGS) != 0))
    {
	report_error("hopscotch", "invalid flags", flags);
	return NULL;
    }
    size_t sz = sizeof(p64_hopscotch_t) +
		sizeof(struct bucket) * nbkts +
		sizeof(struct cell) * ncells;
    p64_hopscotch_t *ht = p64_malloc(sz, CACHE_LINE);
    if (ht != NULL)
    {
	memset(ht, 0, sz);
	ht->cf = cf;
	ht->nbkts = nbkts;
	ht->ncells = ncells;
	ht->use_hp = (flags & P64_HOPSCOTCH_F_HP) != 0;
	ht->cellar = (struct cell *)&ht->buckets[nbkts];
	//All buckets already cleared (NULL elements pointers & null bitmaps)
	//All cells already cleared (NULL element pointers)
    }
    return ht;
}

#undef VALID_FLAGS

void
p64_hopscotch_free(p64_hopscotch_t *ht)
{
    if (ht != NULL)
    {
	//Check if hash table is empty
	for (bix_t i = 0; i < ht->nbkts; i++)
	{
	    //No need to use HP or QSBR, elements are not accessed
	    if (ht->buckets[i].elem != NULL ||
		ht->buckets[i].bmc.bitmap != 0)
	    {
		report_error("hopscotch", "hash table not empty", 0);
		return;
	    }
	}
	for (bix_t i = 0; i < ht->ncells; i++)
	{
	    //No need to use HP or QSBR, elements are not accessed
	    if (ht->cellar[i].elem != NULL)
	    {
		report_error("hopscotch", "hash table not empty", 0);
		return;
	    }
	}
	p64_mfree(ht);
    }
}

static inline void *
search_cellar(p64_hopscotch_t *ht,
	      const void *key,
	      p64_hopschash_t hash,
	      p64_hazardptr_t *hazpp,
	      bool use_hp,
	      bool check_key)
{
    if (UNLIKELY(ht->ncells == 0))
    {
	return NULL;
    }
    bix_t start = ring_mod(hash, ht->ncells);
    //Start search from start position
    bix_t idx = start;
    do
    {
	if (ht->cellar[idx].hash == hash)
	{
	    void *elem = atomic_load_acquire(&ht->cellar[idx].elem,
					     hazpp,
					     use_hp);
	    if (elem != NULL)
	    {
		if (check_key)
		{
		    if (LIKELY(ht->cf(elem, key) == 0))
		    {
			return elem;
		    }
		    //Else false positive
		}
		else//Check key later
		{
		    PREFETCH_FOR_READ(elem);
		    return elem;
		}
	    }
	}
	idx = ring_add(idx, 1, ht->ncells);
    }
    while (idx != start);
    return NULL;
}

static inline uint32_t
hash_to_sig(p64_hopschash_t hash)
{
    return (hash >> 16) & SIG_MASK;
}

ALWAYS_INLINE
static inline void *
lookup(p64_hopscotch_t *ht,
       const void *key,
       p64_hopschash_t hash,
       bix_t bix,
       p64_hazardptr_t *hazpp,
       bool use_hp,
       bool check_key)
{
    union bmc cur;
    cur.atom = atomic_load_n(&ht->buckets[bix].bmc.atom, __ATOMIC_ACQUIRE);
    while (cur.bitmap != 0)
    {
	//Bitmap indicates which buckets in neighbourhood that contain
	//elements which hash to this bucket
	uint32_t bit = __builtin_ctz(cur.bitmap);
	bix_t idx = ring_add(bix, bit, ht->nbkts);
	union bmc elem_bmc;
	void *elem = atomic_load_acquire(&ht->buckets[idx].elem,
					 hazpp,
					 use_hp);
	elem_bmc.atom = atomic_load_n(&ht->buckets[idx].bmc.atom, __ATOMIC_RELAXED);
	if (LIKELY(elem != NULL))
	{
#if SIG_BITS != 0
	    uint32_t sig = hash_to_sig(hash);
	    if (LIKELY(elem_bmc.sig == sig))
#endif
	    {
		if (check_key)
		{
		    if (LIKELY(ht->cf(elem, key) == 0))
		    {
			//Found our element
			//Keep hazard pointer set
			return elem;
		    }
		    //Else false positive
		}
		else//Check key later
		{
		    PREFETCH_FOR_READ(elem);
		    return elem;
		}
	    }
	}
	//Else element just re/moved
	//Clear least significant bit
	cur.bitmap &= cur.bitmap - 1;
	//Else false positive or element has been moved or replaced
	if (cur.bitmap == 0)
	{
	    //Need to check that bmc hasn't changed during our check
	    union bmc fresh;
	    //Prevent re-load of bmc from moving up
	    atomic_thread_fence(__ATOMIC_ACQUIRE);
	    fresh.atom = atomic_load_n(&ht->buckets[bix].bmc.atom, __ATOMIC_RELAXED);
	    if (fresh.count != cur.count)
	    {
		//Bmc has changed, restart with fresh bitmap
		cur = fresh;
		atomic_thread_fence(__ATOMIC_ACQUIRE);
	    }
	}
    }
    if (cur.cellar)
    {
	void *elem = search_cellar(ht, key, hash, hazpp, use_hp, check_key);
	if (elem)
	{
	    //Found our element
	    //Keep hazard pointer set
	    return elem;
	}
    }
    return NULL;
}

void *
p64_hopscotch_lookup(p64_hopscotch_t *ht,
		     const void *key,
		     p64_hopschash_t hash,
		     p64_hazardptr_t *hazpp)
{
    //Caller must call QSBR acquire/release/quiescent as appropriate
    bix_t bix = ring_mod(hash, ht->nbkts);
    PREFETCH_FOR_READ((char *)&ht->buckets[bix]);
    PREFETCH_FOR_READ((char *)&ht->buckets[bix] + CACHE_LINE);
    void *elem = lookup(ht, key, hash, bix, hazpp, ht->use_hp, true);
    return elem;
}

void
p64_hopscotch_lookup_vec(p64_hopscotch_t *ht,
			 uint32_t num,
			 const void *keys[num],
			 p64_hopschash_t hashes[num],
			 void *result[num])
{
    //Caller must call QSBR acquire/release/quiescent as appropriate
    if (UNLIKELY(ht->use_hp))
    {
	report_error("hopscotch", "hazard pointers not supported", 0);
    }
    bix_t bix[num];
    for (uint32_t i = 0; i < num; i++)
    {
	//Compute all bucket indexes
	bix[i] = ring_mod(hashes[i], ht->nbkts);
	//Prefetch bucket metadata
	PREFETCH_FOR_READ((char *)&ht->buckets[bix[i]]);
	if (num <= 8)
	{
	    PREFETCH_FOR_READ((char *)&ht->buckets[bix[i]] + CACHE_LINE);
	}
    }
    for (uint32_t i = 0; i < num; i++)
    {
	result[i] = lookup(ht, keys[i], hashes[i], bix[i], NULL, false, false);
    }
    for (uint32_t i = 0; i < num; i++)
    {
	if (LIKELY(result[i] != NULL))
	{
	    if (ht->cf(result[i], keys[i]) != 0)
	    {
		result[i] = p64_hopscotch_lookup(ht, keys[i], hashes[i], NULL);
	    }
	}
    }
}

static void
bitmap_set_mask(p64_hopscotch_t *ht,
		bix_t bix,//Bucket hash maps to
		bix_t idx)//Bucket element inserted into
{
    uint32_t bit = ring_sub(idx, bix, ht->nbkts);
    assert(bit < BITMAP_BITS);
    uint32_t mask = 1U << bit;
    union bmc old, new;
    do
    {
	old.atom = atomic_load_n(&ht->buckets[bix].bmc.atom, __ATOMIC_RELAXED);
	assert((old.bitmap & mask) == 0);
	new = old;
	new.bitmap |= mask;
	//Always increase change counter
	new.count++;
    }
    while (!atomic_compare_exchange_n(&ht->buckets[bix].bmc.atom,
				      &old.atom,
				      new.atom,
				      __ATOMIC_RELEASE,
				      __ATOMIC_RELAXED));
}

static void
bitmap_update_cellar(p64_hopscotch_t *ht,
		     bix_t bix)
{
    union bmc old, new;
    do
    {
	old.atom = atomic_load_n(&ht->buckets[bix].bmc.atom, __ATOMIC_ACQUIRE);
	new = old;
	new.cellar = 0;
	for (bix_t i = 0; i < ht->ncells; i++)
	{
	    p64_hopschash_t hash =
		atomic_load_n(&ht->cellar[i].hash, __ATOMIC_RELAXED);
	    void *elem =
		atomic_load_ptr(&ht->cellar[i].elem, __ATOMIC_RELAXED);
	    if (elem != NULL && ring_mod(hash, ht->nbkts) == bix)
	    {
		//Found another element which hashes to same bix
		new.cellar = 1;
		break;
	    }
	}
	if (new.cellar == old.cellar)
	{
	    //No need to update
	    break;
	}
	//Always increase change counter
	new.count++;
	//Attempt to update bmc, fail and retry if count has changed
    }
    while (!atomic_compare_exchange_n(&ht->buckets[bix].bmc.atom,
				      &old.atom,
				      new.atom,
				      __ATOMIC_RELEASE,
				      __ATOMIC_RELAXED));
}

static bool
find_empty_bkt(p64_hopscotch_t *ht,
	       bix_t bix,
	       bix_t *empty)
{
    bix_t idx = bix;
    do
    {
	void *elem = atomic_load_ptr(&ht->buckets[idx].elem, __ATOMIC_RELAXED);
	if (elem == NULL)
	{
	    *empty = idx;
	    return true;
	}
	idx = ring_add(idx, 1, ht->nbkts);
    }
    while (idx != bix);
    //Searched all of hash table
    return false;
}

static bool
find_move_candidate(p64_hopscotch_t *ht,
		    bix_t empty,
		    bix_t *home_bix,
		    union bmc *home_bmc,
		    bix_t *src_idx)
{
    for (uint32_t i = BITMAP_BITS - 1; i != 0; i--)
    {
	bix_t bix = ring_sub(empty, i, ht->nbkts);
	//Assert empty is in the neighbourhood of bix
	assert(ring_sub(empty, bix, ht->nbkts) < BITMAP_BITS);
	union bmc bmc;
	bmc.atom = atomic_load_n(&ht->buckets[bix].bmc.atom, __ATOMIC_ACQUIRE);
	if (bmc.bitmap != 0)
	{
	    //Check first element that hash to bix bucket
	    uint32_t bit = __builtin_ctz(bmc.bitmap);
	    //Test if bix + bit is before empty
	    bix_t dist_b2e = ring_sub(empty, bix, ht->nbkts);
	    if (bit < dist_b2e)
	    {
		//Found element closer to bix which can be moved to empty bucket
		*home_bix = bix;
		*src_idx = ring_add(bix, bit, ht->nbkts);
		*home_bmc = bmc;
		return true;
	    }
	    //Else bix+bit is after empty and so is any other set bits in bitmap
	}
	//Else no candidates for this bucket
    }
    return false;
}

static inline bool
write_elem(struct bucket *bkt,
	   void *elem,
	   uint32_t sig,
	   bool home_bkt,//Writing element to its home bucket
	   bool rls)
{
#if SIG_BITS == 0
    (void)sig;
#endif
    union
    {
	struct bucket bk;
	ptrpair_t pp;
    } old, new;
    do
    {
#if 1
//Using atomic loads sometimes leads to 35-50ns more overhead
	old.bk.bmc.atom = atomic_load_n(&bkt->bmc.atom, __ATOMIC_RELAXED);
	old.bk.elem = atomic_load_ptr(&bkt->elem, __ATOMIC_RELAXED);
#else//Let's use plain loads
	old.bk = *bkt;
#endif
	if (UNLIKELY(old.bk.elem != NULL))
	{
	    //Slot not empty anymore
	    return false;
	}
	old.bk.elem = NULL;
	new.bk.bmc = old.bk.bmc;
	new.bk.bmc.bitmap |= home_bkt;
#if SIG_BITS != 0
	new.bk.bmc.sig = sig;
#endif
	new.bk.bmc.count += home_bkt;
	new.bk.elem = elem;
    }
    while (UNLIKELY(!atomic_compare_exchange_n((ptrpair_t*)bkt,
					       &old.pp,
					       new.pp,
					       rls ? __ATOMIC_RELEASE : __ATOMIC_RELAXED,
					       __ATOMIC_RELAXED)));
    return true;
}

enum move_result { ht_full, dst_no_empty, move_ok };

static enum move_result
move_elem(p64_hopscotch_t *ht,
	  bix_t *empty)
{
    bix_t dst_idx = *empty;
    for (;;)
    {
	//Find source bucket which has empty bucket in its neighbourhood
	bix_t home_bix, src_idx;
	union bmc home_bmc;
	if (!find_move_candidate(ht, dst_idx, &home_bix, &home_bmc, &src_idx))
	{
	    //Found no candidate which can be moved to empty bucket
	    return ht_full;
	}
	//Copy element from source to empty bucket
	struct bucket src;
#if 0
	src.bmc.atom = atomic_load_n(&ht->buckets[src_idx].bmc.atom, __ATOMIC_RELAXED);
	src.elem = atomic_load_ptr(&ht->buckets[src_idx].elem, __ATOMIC_RELAXED);
#else
	src = ht->buckets[src_idx];
#endif
#if SIG_BITS != 0
	uint32_t src_sig = src.bmc.sig;
#else
	uint32_t src_sig = 0;
#endif
	if (UNLIKELY(!write_elem(&ht->buckets[dst_idx],
				 src.elem,
				 src_sig,
				 false,
				 false)))
	{
	    //Bucket not empty anymore
	    return dst_no_empty;
	}
	//Update home_bix bitmap to reflect move
	union bmc new_bmc;
	new_bmc = home_bmc;
	uint32_t src_bit = ring_sub(src_idx, home_bix, ht->nbkts);
	assert(src_bit < BITMAP_BITS);
	uint32_t dst_bit = ring_sub(dst_idx, home_bix, ht->nbkts);
	assert(dst_bit < BITMAP_BITS);
	assert(dst_bit > src_bit);
	//Clear source bit
	new_bmc.bitmap &= ~(1U << src_bit);
	//Set destination bit
	new_bmc.bitmap |= 1U << dst_bit;
	//Always increase change counter
	new_bmc.count++;
	if (atomic_compare_exchange_n(&ht->buckets[home_bix].bmc.atom,
				      &home_bmc.atom,
				      new_bmc.atom,
				      __ATOMIC_RELEASE,
				      __ATOMIC_RELAXED))
	{
	    //Moved element from source to empty
	    //Clear source bucket
	    atomic_store_ptr(&ht->buckets[src_idx].elem,
			     NULL, __ATOMIC_RELAXED);
	    *empty = src_idx;
	    return move_ok;
	}
	//Else home_bix bitmap changed, our element could have been moved
	//Undo move
	atomic_store_ptr(&ht->buckets[dst_idx].elem, NULL, __ATOMIC_RELAXED);
	//Restart from beginning
    }
}

static bool
insert_bkt(p64_hopscotch_t *ht,
	   void *elem,
	   p64_hopschash_t hash)
{
    bix_t bix = ring_mod(hash, ht->nbkts);
    bix_t empty;
    do
    {
find_empty:
	if (UNLIKELY(!find_empty_bkt(ht, bix, &empty)))
	{
	    return false;//Hash table full
	}
	while (ring_sub(empty, bix, ht->nbkts) >= BITMAP_BITS)
	{
	    //Else empty bucket not in neighbourhood
	    enum move_result mr = move_elem(ht, &empty);
	    if (mr == ht_full)
	    {
		return false;
	    }
	    else if (mr == dst_no_empty)
	    {
		goto find_empty;
	    }
	}
	assert(ring_sub(empty, bix, ht->nbkts) < BITMAP_BITS);
	//Empty bucket is in neighbourhood
	//Insert new element into empty bucket
    }
    while (UNLIKELY(!write_elem(&ht->buckets[empty],
				elem,
				hash_to_sig(hash),
				empty == bix,
				true)));
    if (empty != bix)
    {
	//Must also update home bucket.bmc
	bitmap_set_mask(ht, bix, empty);
    }
    return true;
}

static bool
insert_cell(p64_hopscotch_t *ht,
	    void *elem,
	    p64_hopschash_t hash)
{
    if (UNLIKELY(ht->ncells == 0))
    {
	return false;
    }
    bix_t start = ring_mod(hash, ht->ncells);
    bix_t idx = start;
    do
    {
	if (atomic_load_ptr(&ht->cellar[idx].elem, __ATOMIC_RELAXED) == NULL)
	{
	    void *old = NULL;
	    if (atomic_compare_exchange_ptr(&ht->cellar[idx].elem,
					    &old,
					    elem,
					    __ATOMIC_RELEASE,
					    __ATOMIC_RELAXED))
	    {
		atomic_store_n(&ht->cellar[idx].hash, hash, __ATOMIC_RELAXED);
		//Set cellar bit in bix bucket to indicate present of element
		//in cellar
		bix_t bix = ring_mod(hash, ht->nbkts);
		union bmc old, new;
		do
		{
		    old.atom = atomic_load_n(&ht->buckets[bix].bmc.atom,
					   __ATOMIC_RELAXED);
		    new = old;
		    new.cellar = 1;
		    new.count++;
		}
		while (!atomic_compare_exchange_n(&ht->buckets[bix].bmc.atom,
						  &old.atom,
						  new.atom,
						  __ATOMIC_RELEASE,
						  __ATOMIC_RELAXED));
		return true;
	    }
	}
	idx = ring_add(idx, 1, ht->ncells);
    }
    while (idx != start);
    return false;
}

bool
p64_hopscotch_insert(p64_hopscotch_t *ht,
		     void *elem,
		     p64_hopschash_t hash)
{
    if (LIKELY(!ht->use_hp))
    {
	p64_qsbr_acquire();
    }
    bool success = insert_bkt(ht, elem, hash);
    if (UNLIKELY(!success))
    {
	success = insert_cell(ht, elem, hash);
    }
    if (LIKELY(!ht->use_hp))
    {
	p64_qsbr_release();
    }
    return success;
}

static bool
remove_bkt_by_ptr(p64_hopscotch_t *ht,
		  void *rem_elem,
		  p64_hopschash_t hash)
{
    bix_t bix = ring_mod(hash, ht->nbkts);
    uint32_t prev_count;
    union bmc cur;
    cur.atom = atomic_load_n(&ht->buckets[bix].bmc.atom, __ATOMIC_ACQUIRE);
    do
    {
	if (cur.bitmap == 0)
	{
	    break;
	}
	union bmc old = cur;
	while (cur.bitmap != 0)
	{
	    //Bitmap indicates which buckets in neighbourhood that contain
	    //elements which hash this bucket
	    uint32_t bit = __builtin_ctz(cur.bitmap);
	    bix_t idx = (bix + bit) % ht->nbkts;
	    if (atomic_load_ptr(&ht->buckets[idx].elem, __ATOMIC_RELAXED) ==
				rem_elem)
	    {
		//Found our element
		union bmc new = old;
		//Clear bit in bix bitmap
		new.bitmap &= ~(1U << bit);
		//Always increase change counter
		new.count++;
		//TODO loop because bmc.sig might have changed
		if (atomic_compare_exchange_n(&ht->buckets[bix].bmc.atom,
					      &old.atom,//Updated on failure
					      new.atom,
					      __ATOMIC_RELEASE,
					      __ATOMIC_RELAXED))
		{
		    //Successfully updated bitmap, now clear bucket
		    atomic_store_ptr(&ht->buckets[idx].elem,
				     NULL,
				     __ATOMIC_RELAXED);
		    return true;
		}
		//Else bitmap changed
		break;//Quit inner loop early
	    }
	    //Clear least significant bit
	    cur.bitmap &= cur.bitmap - 1;
	}
	prev_count = cur.count;
	//Prevent re-load of bmc from moving up
	atomic_thread_fence(__ATOMIC_ACQUIRE);
	cur.atom = atomic_load_n(&ht->buckets[bix].bmc.atom, __ATOMIC_ACQUIRE);
    }
    while (cur.count != prev_count);
    return false;
}

static bool
remove_cell_by_ptr(p64_hopscotch_t *ht,
		   void *elem,
		   p64_hopschash_t hash)
{
    if (UNLIKELY(ht->ncells == 0))
    {
	return NULL;
    }
    bix_t start = ring_mod(hash, ht->ncells);
    bix_t idx = start;
    do
    {
	if (atomic_load_ptr(&ht->cellar[idx].elem, __ATOMIC_RELAXED) == elem)
	{
	    void *old = elem;
	    if (atomic_compare_exchange_ptr(&ht->cellar[idx].elem,
					    &old,
					    NULL,
					    __ATOMIC_RELAXED,
					    __ATOMIC_RELAXED))
	    {
		//Element pointer removed
		//Let hash value remain to avoid race condition with insert
		bitmap_update_cellar(ht, ring_mod(hash, ht->nbkts));
		return true;
	    }
	}
	idx = ring_add(idx, 1, ht->ncells);
    }
    while (idx != start);
    return false;
}

bool
p64_hopscotch_remove(p64_hopscotch_t *ht,
		     void *elem,
		     p64_hopschash_t hash)
{
    if (LIKELY(!ht->use_hp))
    {
	p64_qsbr_acquire();
    }
    bool success = remove_bkt_by_ptr(ht, elem, hash);
    if (UNLIKELY(!success))
    {
	success = remove_cell_by_ptr(ht, elem, hash);
    }
    if (LIKELY(!ht->use_hp))
    {
	p64_qsbr_release();
    }
    return success;
}

static void *
remove_bkt_by_key(p64_hopscotch_t *ht,
		  const void *key,
		  p64_hopschash_t hash,
		  p64_hazardptr_t *hazpp)
{
    uint32_t prev_count;
    bix_t bix = ring_mod(hash, ht->nbkts);
    union bmc cur;
    cur.atom = atomic_load_n(&ht->buckets[bix].bmc.atom, __ATOMIC_ACQUIRE);
    do
    {
	if (cur.bitmap == 0)
	{
	    break;
	}
	union bmc old = cur;
	while (cur.bitmap != 0)
	{
	    //Bitmap indicates which buckets in neighbourhood that contain
	    //elements which hash this bucket
	    uint32_t bit = __builtin_ctz(cur.bitmap);
	    bix_t idx = (bix + bit) % ht->nbkts;
	    union bmc elem_bmc;
	    elem_bmc.atom = atomic_load_n(&ht->buckets[idx].bmc.atom, __ATOMIC_RELAXED);
	    void *elem = atomic_load_acquire(&ht->buckets[idx].elem,
					     hazpp,
					     ht->use_hp);
	    if (LIKELY(elem != NULL))
	    {
#if SIG_BITS != 0
		uint32_t sig = hash_to_sig(hash);
		if (elem_bmc.sig == sig)
#endif
		{
		    if (LIKELY(ht->cf(elem, key) == 0))
		    {
			//Found our element
			union bmc new = old;
			//Clear bit in bix bitmap
			new.bitmap &= ~(1U << bit);
			//Always increase change counter
			new.count++;
			if (atomic_compare_exchange_n(&ht->buckets[bix].bmc.atom,
						      &old.atom,
						      new.atom,
						      __ATOMIC_RELAXED,
						      __ATOMIC_RELAXED))
			{
			    //Successfully updated bitmap, now clear bucket
			    atomic_store_ptr(&ht->buckets[idx].elem,
					     NULL,
					     __ATOMIC_RELAXED);
			    return elem;
			}
			//Else bitmap changed
			break;//Quit inner loop early
		    }
		    //Else false positive
		}
	    }
	    //Clear least significant bit
	    cur.bitmap &= cur.bitmap - 1;
	}
	prev_count = cur.count;
	//Prevent re-load of bmc from moving up
	atomic_thread_fence(__ATOMIC_ACQUIRE);
	cur.atom = atomic_load_n(&ht->buckets[bix].bmc.atom, __ATOMIC_ACQUIRE);
    }
    while (cur.count != prev_count);
    p64_hazptr_release(hazpp);
    return NULL;
}

static void *
remove_cell_by_key(p64_hopscotch_t *ht,
		   const void *key,
		   p64_hopschash_t hash,
		   p64_hazardptr_t *hazpp)

{
    if (UNLIKELY(ht->ncells == 0))
    {
	return NULL;
    }
    bix_t start = ring_mod(hash, ht->ncells);
    bix_t idx = start;
    do
    {
	if (atomic_load_n(&ht->cellar[idx].hash, __ATOMIC_ACQUIRE) == hash)
	{
	    void *elem = atomic_load_acquire(&ht->cellar[idx].elem,
					     hazpp,
					     ht->use_hp);
	    if (ht->cf(key, elem) == 0)
	    {
		if (atomic_compare_exchange_ptr(&ht->cellar[idx].elem,
						&elem,
						NULL,
						__ATOMIC_RELAXED,
						__ATOMIC_RELAXED))
		{
		    //Element pointer removed
		    //Let hash value remain to avoid race condition with insert
		    bitmap_update_cellar(ht, ring_mod(hash, ht->nbkts));
		    return elem;
		}
		//Else cell changed, element removed
	    }
	    //Else key does not match (false positive)
	}
	//Else hash does not match (true negative)
	idx = ring_add(idx, 1, ht->ncells);
    }
    while (idx != start);
    atomic_ptr_release(hazpp, ht->use_hp);
    return NULL;
}

void *
p64_hopscotch_remove_by_key(p64_hopscotch_t *ht,
			    const void *key,
			    p64_hopschash_t hash,
			    p64_hazardptr_t *hazpp)
{
    //Caller must call QSBR acquire/release/quiescent as appropriate
    if (LIKELY(!ht->use_hp))
    {
	p64_qsbr_acquire();
    }
    void *elem = remove_bkt_by_key(ht, key, hash, hazpp);
    if (UNLIKELY(!elem))
    {
	elem = remove_cell_by_key(ht, key, hash, hazpp);
    }
    if (LIKELY(!ht->use_hp))
    {
	p64_qsbr_release();
    }
    return elem;
}
