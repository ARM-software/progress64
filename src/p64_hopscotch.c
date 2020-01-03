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
#include "arch.h"
#include "os_abstraction.h"
#include "err_hnd.h"

typedef size_t index_t;

//CELLAR_BIT is used in traverse call-back to indicate index refers to cellar
#define CELLAR_BIT ((size_t)1 << (sizeof(size_t) * CHAR_BIT - 1))

static inline index_t
ring_add(index_t a, index_t b, index_t modulo)
{
    assert(a < modulo);
    assert(b < modulo);
    index_t sum = a + b;
    if (UNLIKELY(sum >= modulo))
    {
	sum -= modulo;
    }
    assert(sum < modulo);
    return sum;
}

static inline index_t
ring_sub(index_t a, index_t b, index_t modulo)
{
    assert(a < modulo);
    assert(b < modulo);
    index_t dif = a - b;
    if (UNLIKELY(dif >= modulo))
    {
	dif += modulo;
    }
    assert(dif < modulo);
    return dif;
}

static inline index_t
ring_mod(p64_hopschash_t hash, index_t modulo)
{
    index_t rem = hash % modulo;
    assert(rem < modulo);
    return rem;
}

#if __SIZEOF_POINTER__ == 4
#define BITMAP_BITS 16
#define COUNT_BITS 15
typedef uint16_t bitmap_t;
#else //__SIZEOF_POINTER__ == 8
#define BITMAP_BITS 32
#define COUNT_BITS 31
typedef uint32_t bitmap_t;
#endif

struct bmc
{
    bitmap_t bitmap;//Neighbourhood map of elements which hash to this bucket
    unsigned cellar:1;//True if elements stored in cellar
    unsigned count:COUNT_BITS;//Change counter
};

struct cell
{
    p64_hopschash_t hash;
    void *elem;
};

struct bucket
{
    struct bmc bmc;
    void *elem;
};

struct p64_hopscotch
{
    p64_hopscotch_compare cf;
    index_t nbkts;
    index_t ncells;
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

void
p64_hopscotch_traverse(p64_hopscotch_t *ht,
		       p64_hopscotch_trav_cb cb,
		       void *arg)
{
    for (index_t idx = 0; idx < ht->nbkts; idx++)
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
    for (index_t idx = 0; idx < ht->ncells; idx++)
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
    size_t histo[BITMAP_BITS] = { 0 };
    size_t nfull = 0;
    size_t nelems = 0;
    bitmap_t sliding_bm = 0;
    //Initialise sliding_bm from last BITMAP_BITS buckets
    for (index_t bix = ht->nbkts - BITMAP_BITS; bix < ht->nbkts; bix++)
    {
	bitmap_t bix_bm = ht->buckets[bix].bmc.bitmap;
	sliding_bm >>= 1;
	sliding_bm |= bix_bm;
    }
    for (index_t bix = 0; bix < ht->nbkts; bix++)
    {
	bitmap_t bix_bm = ht->buckets[bix].bmc.bitmap;
	sliding_bm >>= 1;
	if ((sliding_bm & bix_bm) != 0)
	{
	    fprintf(stderr, "%zu: (sliding_bm & bix_bm) != 0\n", bix);
	}
	sliding_bm |= bix_bm;
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
	if (sliding_bm == (bitmap_t)~0)
	{
	    nfull++;
	}
	while (bix_bm != 0)
	{
	    uint32_t bit = __builtin_ctz(bix_bm);
	    bix_bm &= ~(1U << bit);
	    histo[bit]++;
	}
    }
    size_t ncellar = 0;
    for (index_t i = 0; i < ht->ncells; i++)
    {
	nelems += (ht->cellar[i].elem != NULL);
	ncellar += (ht->cellar[i].elem != NULL);
    }
    printf("Hopscotch hash table: %zu buckets, %zu cells, "
	    "%zu elements, load=%.2f\n",
	    ht->nbkts, ht->ncells,
	    nelems, (float)nelems / (ht->nbkts + ht->ncells));
    printf("Neighbourhood distance histogram:\n");
    for (uint32_t i = 0; i < BITMAP_BITS / 2; i++)
    {
	printf("%2u: %6zu (%.3f)\t\t%2u: %6zu (%.3f)\n",
		i, histo[i], histo[i] / (float)nelems,
		i + BITMAP_BITS / 2, histo[i + BITMAP_BITS / 2],
		histo[i + BITMAP_BITS / 2] / (float)nelems);
    }
    printf("Cellar: %zu\n", ncellar);
    printf("%zu neighbourhoods are completely full\n", nfull);
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
	for (index_t i = 0; i < ht->nbkts; i++)
	{
	    //No need to use HP or QSBR, elements are not accessed
	    if (ht->buckets[i].elem != NULL ||
		ht->buckets[i].bmc.bitmap != 0)
	    {
		report_error("hopscotch", "hash table not empty", 0);
		return;
	    }
	}
	for (index_t i = 0; i < ht->ncells; i++)
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
	      p64_hazardptr_t *hazpp)
{
    if (UNLIKELY(ht->ncells == 0))
    {
	return NULL;
    }
    index_t start = ring_mod(hash, ht->ncells);
    //Start search from start position
    index_t idx = start;
    do
    {
	if (ht->cellar[idx].hash == hash)
	{
	    void *elem = atomic_load_acquire(&ht->cellar[idx].elem,
					     hazpp,
					     hazpp != NULL && ht->use_hp);
	    if (elem != NULL && ht->cf(elem, key) == 0)
	    {
		return elem;
	    }
	}
	idx = ring_add(idx, 1, ht->ncells);
    }
    while (idx != start);
    return NULL;
}

void *
p64_hopscotch_lookup(p64_hopscotch_t *ht,
		     const void *key,
		     p64_hopschash_t hash,
		     p64_hazardptr_t *hazpp)
{
    //Caller must call QSBR acquire/release/quiescent as appropriate
    index_t bix = ring_mod(hash, ht->nbkts);
    PREFETCH_FOR_READ((char *)&ht->buckets[bix]);
    PREFETCH_FOR_READ((char *)&ht->buckets[bix] + CACHE_LINE);
    struct bmc cur;
    __atomic_load(&ht->buckets[bix].bmc, &cur, __ATOMIC_ACQUIRE);
    while (cur.bitmap != 0)
    {
	//Bitmap indicates which buckets in neighbourhood that contain
	//elements which hash to this bucket
	uint32_t bit = __builtin_ctz(cur.bitmap);
	index_t idx = ring_add(bix, bit, ht->nbkts);
	void *elem = atomic_load_acquire(&ht->buckets[idx].elem,
					 hazpp,
					 ht->use_hp);
	if (LIKELY(elem != NULL))
	{
	    if (LIKELY(ht->cf(elem, key) == 0))
	    {
		//Found our element
		//Keep hazard pointer set
		return elem;
	    }
	    //Else false positive
	}
	//Else element just re/moved
	cur.bitmap &= ~(1U << bit);
	//Else false positive or element has been moved or replaced
	if (cur.bitmap == 0)
	{
	    //Need to check that bmc hasn't changed during our check
	    struct bmc fresh;
	    //Prevent re-load of bmc from moving up
	    __atomic_thread_fence(__ATOMIC_ACQUIRE);
	    __atomic_load(&ht->buckets[bix].bmc, &fresh, __ATOMIC_RELAXED);
	    if (fresh.count != cur.count)
	    {
		//Bmc has changed, restart with fresh bitmap
		cur = fresh;
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
	    }
	}
    }
    if (cur.cellar)
    {
	void *elem = search_cellar(ht, key, hash, hazpp);
	if (elem != NULL)
	{
	    //Found our element
	    //Keep hazard pointer set
	    return elem;
	}
    }
    return NULL;
}

static unsigned long
load_elems(p64_hopscotch_t *ht,
	   unsigned long mask,
	   size_t bix[],
	   struct bmc cur[],
	   void *result[])
{
    unsigned long next_mask = 0;
    do
    {
	uint32_t i = __builtin_ctz(mask);
	mask &= ~(1UL << i);
	if (cur[i].bitmap != 0)
	{
	    uint32_t bit = __builtin_ctz(cur[i].bitmap);
	    //Keep bit in bitmap, we need to find it later
	    index_t idx = ring_add(bix[i], bit, ht->nbkts);
	    result[i] = __atomic_load_n(&ht->buckets[idx].elem,
					__ATOMIC_RELAXED);
	    next_mask |= 1UL << i;
	}
    }
    while (LIKELY(mask != 0));
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return next_mask;
}

static unsigned long
check_elems(p64_hopscotch_t *ht,
	    unsigned long mask,
	    const void *keys[],
	    size_t bix[],
	    struct bmc cur[],
	    void *result[],
	    unsigned long *success)
{
    unsigned long next_mask = 0;
    do
    {
	uint32_t i = __builtin_ctz(mask);
	mask &= ~(1UL << i);
	//This bit in neighbourhood has been processed so clear it
	assert(cur[i].bitmap != 0);
	uint32_t bit = __builtin_ctz(cur[i].bitmap);
	cur[i].bitmap &= ~(1U << bit);

	if (result[i] != NULL)
	{
	    if (ht->cf(result[i], keys[i]) == 0)
	    {
		//Found our element
		*success |= 1UL << i;
		continue;
	    }
	    //Else False positive
	    result[i] = NULL;
	}
	if (cur[i].bitmap == 0)
	{
	    //No more neighbours to check
	    struct bmc fresh;
	    __atomic_load(&ht->buckets[bix[i]].bmc, &fresh, __ATOMIC_RELAXED);
	    if (fresh.count != cur[i].count && fresh.bitmap != 0)
	    {
		//Bitmap has changed since we began so start over
		cur[i] = fresh;
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		next_mask |= 1UL << i;
	    }
	    //Else bitmap hasn't changed or has become empty
	}
	else//More elements in neighbourhood to check
	{
	    next_mask |= 1UL << i;
	}
    }
    while (LIKELY(mask != 0));
    return next_mask;
}

unsigned long
p64_hopscotch_lookup_vec(p64_hopscotch_t *ht,
			 uint32_t num,
			 const void *keys[num],
			 p64_hopschash_t hashes[num],
			 void *result[num])
{
    uint32_t long_bits = sizeof(long) * CHAR_BIT;
    if (UNLIKELY(num > long_bits))
    {
	report_error("hopscotch", "invalid vector size", num);
	return 0;
    }
    if (UNLIKELY(ht->use_hp))
    {
	report_error("hopscotch", "hazard pointers not supported", 0);
	return 0;
    }
    index_t bix[num];
    struct bmc cur[num];
    for (uint32_t i = 0; i < num; i++)
    {
	//Compute all hash bucket indexes
	bix[i] = ring_mod(hashes[i], ht->nbkts);
	//Load all bitmaps:counters
	__atomic_load(&ht->buckets[bix[i]].bmc, &cur[i], __ATOMIC_RELAXED);
	//Initialise results
	result[i] = NULL;
    }
    unsigned long mask = num == long_bits ? ~0UL : (1UL << num) - 1;
    unsigned long success = 0;//Success bitmap
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    while (mask != 0)
    {
	mask = load_elems(ht, mask, bix, cur, result);
	if (mask == 0)
	{
	    break;
	}
	mask = check_elems(ht, mask, keys, bix, cur, result, &success);
    }
    //Search the cellar for any misses
    mask = num == long_bits ? ~0UL : (1UL << num) - 1;
    if (success != mask)
    {
	mask &= ~success;//Bit mask of failures
	assert(mask != 0);
	do
	{
	    uint32_t i = __builtin_ctz(mask);
	    mask &= ~(1UL << i);
	    if (cur[i].cellar)
	    {
		void *elem = search_cellar(ht, keys[i], hashes[i], NULL);
		if (elem != NULL)
		{
		    result[i] = elem;
		    success |= 1UL << i;
		}
	    }
	}
	while (mask != 0);
    }
    return success;
}

static void
bitmap_set_mask(p64_hopscotch_t *ht,
		index_t bix,//Bucket hash maps to
		index_t idx)//Bucket element inserted into
{
    struct bmc old, new;
    uint32_t bit = ring_sub(idx, bix, ht->nbkts);
    assert(bit < BITMAP_BITS);
    bitmap_t mask = 1U << bit;
    //TODO use atomic_add? requires bmc to be scalar or union
    do
    {
	__atomic_load(&ht->buckets[bix].bmc, &old, __ATOMIC_RELAXED);
	assert((old.bitmap & mask) == 0);
	new.bitmap = old.bitmap | mask;
	new.cellar = old.cellar;
	new.count = old.count + 1;
    }
    while (!__atomic_compare_exchange(&ht->buckets[bix].bmc,
				      &old,
				      &new,
				      /*weak=*/false,
				      __ATOMIC_RELEASE,
				      __ATOMIC_RELAXED));
}

static void
bitmap_update_cellar(p64_hopscotch_t *ht,
		     index_t bix)
{
    struct bmc old, new;
    do
    {
	__atomic_load(&ht->buckets[bix].bmc, &old, __ATOMIC_ACQUIRE);
	new = old;
	new.cellar = 0;
	for (index_t i = 0; i < ht->ncells; i++)
	{
	    p64_hopschash_t hash =
		__atomic_load_n(&ht->cellar[i].hash, __ATOMIC_RELAXED);
	    void *elem =
		__atomic_load_n(&ht->cellar[i].elem, __ATOMIC_RELAXED);
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
	new.count++;
	//Attempt to update bmc, fail if count has changed
    }
    while (!__atomic_compare_exchange(&ht->buckets[bix].bmc,
				      &old,
				      &new,
				      /*weak=*/false,
				      __ATOMIC_RELEASE,
				      __ATOMIC_RELAXED));
}

static bool
find_empty_bkt(p64_hopscotch_t *ht,
	       index_t bix,
	       index_t *empty)
{
    index_t idx = bix;
    do
    {
	void *elem = __atomic_load_n(&ht->buckets[idx].elem, __ATOMIC_RELAXED);
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
		    index_t empty,
		    index_t *src_bix,
		    index_t *src_idx,
		    struct bmc *src_bmc)
{
    for (uint32_t i = BITMAP_BITS - 1; i != 0; i--)
    {
	index_t bix = ring_sub(empty, i, ht->nbkts);
	//Assert empty is in the neighbourhood of bix
	assert(ring_sub(empty, bix, ht->nbkts) < BITMAP_BITS);
	struct bmc bmc;
	__atomic_load(&ht->buckets[bix].bmc, &bmc, __ATOMIC_ACQUIRE);
	if (bmc.bitmap != 0)
	{
	    //Check first element that hash to bix bucket
	    uint32_t bit = __builtin_ctz(bmc.bitmap);
	    //Test if bix + bit is before empty
	    index_t dist_b2e = ring_sub(empty, bix, ht->nbkts);
	    if (bit < dist_b2e)
	    {
		//Found element closer to bix which can be moved to empty bucket
		*src_bix = bix;
		*src_idx = ring_add(bix, bit, ht->nbkts);
		*src_bmc = bmc;
		return true;
	    }
	    //Else bix+bit is after empty and so is any other set bits in bitmap
	}
	//Else no candidates for this bucket
    }
    return false;
}

enum move_result { ht_full, dst_no_empty, move_ok };

static enum move_result
move_elem(p64_hopscotch_t *ht,
	  index_t *empty)
{
    index_t dst_idx = *empty;
    for (;;)
    {
	//Find source bucket which has empty bucket in its neighbourhood
	index_t src_bix, src_idx;
	struct bmc src_bmc;
	if (!find_move_candidate(ht, dst_idx, &src_bix, &src_idx, &src_bmc))
	{
	    //Found no candidate which can be moved to empty bucket
	    return ht_full;
	}
	//Copy element from source to empty bucket
	void *move_elem = __atomic_load_n(&ht->buckets[src_idx].elem,
					  __ATOMIC_RELAXED);
	void *old = NULL;
	if (!__atomic_compare_exchange_n(&ht->buckets[dst_idx].elem,
					 &old,
					 move_elem,
					 /*weak=*/false,
					 __ATOMIC_RELAXED,
					 __ATOMIC_RELAXED))
	{
	    //Bucket not empty anymore
	    return dst_no_empty;
	}
	//Update src_bix bitmap to reflect move
	struct bmc new_bmc;
	new_bmc.bitmap = src_bmc.bitmap;
	uint32_t src_bit = ring_sub(src_idx, src_bix, ht->nbkts);
	assert(src_bit < BITMAP_BITS);
	uint32_t dst_bit = ring_sub(dst_idx, src_bix, ht->nbkts);
	assert(dst_bit < BITMAP_BITS);
	assert(dst_bit > src_bit);
	//Clear source bit
	new_bmc.bitmap &= ~(1U << src_bit);
	//Set destination bit
	new_bmc.bitmap |= 1U << dst_bit;
	new_bmc.cellar = src_bmc.cellar;
	new_bmc.count = src_bmc.count + 1;
	if (__atomic_compare_exchange(&ht->buckets[src_bix].bmc,
				      &src_bmc,
				      &new_bmc,
				      /*weak=*/false,
				      __ATOMIC_RELEASE,
				      __ATOMIC_RELAXED))
	{
	    //Moved element from source to empty
	    //Clear source bucket
	    __atomic_store_n(&ht->buckets[src_idx].elem,
			     NULL, __ATOMIC_RELAXED);
	    *empty = src_idx;
	    return move_ok;
	}
	//Else src_bix bitmap changed, our element could have been moved
	//Undo move
	__atomic_store_n(&ht->buckets[dst_idx].elem, NULL, __ATOMIC_RELAXED);
	//Restart from beginning
    }
}

static bool
insert_bkt(p64_hopscotch_t *ht,
	   void *elem,
	   index_t bix)
{
    for (;;)
    {
	index_t empty;
find_empty:
	if (!find_empty_bkt(ht, bix, &empty))
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
	void *old = NULL;
	if (__atomic_compare_exchange_n(&ht->buckets[empty].elem,
					&old,
					elem,
					/*weak=*/false,
					__ATOMIC_RELEASE,
					__ATOMIC_RELAXED))
	{
	    bitmap_set_mask(ht, bix, empty);
	    return true;
	}
	//Else bucket not empty anymore
    }
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
    index_t start = ring_mod(hash, ht->ncells);
    index_t idx = start;
    do
    {
	if (__atomic_load_n(&ht->cellar[idx].elem, __ATOMIC_RELAXED) == NULL)
	{
	    void *old = NULL;
	    if (__atomic_compare_exchange_n(&ht->cellar[idx].elem,
					    &old,
					    elem,
					    /*weak=*/false,
					    __ATOMIC_RELEASE,
					    __ATOMIC_RELAXED))
	    {
		__atomic_store_n(&ht->cellar[idx].hash, hash, __ATOMIC_RELAXED);
		//Set cellar bit in bix bucket to indicate present of element
		//in cellar
		index_t bix = ring_mod(hash, ht->nbkts);
		struct bmc old, new;
		do
		{
		    __atomic_load(&ht->buckets[bix].bmc,
				  &old, __ATOMIC_RELAXED);
		    new = old;
		    new.cellar = 1;
		    new.count++;
		}
		while (!__atomic_compare_exchange(&ht->buckets[bix].bmc,
						  &old,
						  &new,
						  /*weak=*/false,
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
    bool success = insert_bkt(ht, elem, ring_mod(hash, ht->nbkts));
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
		  index_t bix)
{
    uint32_t prev_count;
    struct bmc cur;
    __atomic_load(&ht->buckets[bix].bmc, &cur, __ATOMIC_ACQUIRE);
    do
    {
	if (cur.bitmap == 0)
	{
	    break;
	}
	struct bmc old = cur;
	while (cur.bitmap != 0)
	{
	    //Bitmap indicates which buckets in neighbourhood that contain
	    //elements which hash this bucket
	    uint32_t bit = __builtin_ctz(cur.bitmap);
	    index_t idx = (bix + bit) % ht->nbkts;
	    if (__atomic_load_n(&ht->buckets[idx].elem, __ATOMIC_RELAXED) ==
				rem_elem)
	    {
		//Found our element
		//Clear bit in bix bitmap
		struct bmc new;
		new.bitmap = old.bitmap & ~(1U << bit);
		new.cellar = old.cellar;
		new.count = old.count + 1;
		if (__atomic_compare_exchange(&ht->buckets[bix].bmc,
					      &old,//Updated on failure
					      &new,
					      /*weak=*/false,
					      __ATOMIC_RELAXED,
					      __ATOMIC_RELAXED))
		{
		    //Successfully updated bitmap, now clear bucket
		    __atomic_store_n(&ht->buckets[idx].elem,
				     NULL,
				     __ATOMIC_RELAXED);
		    return true;
		}
		//Else bitmap changed
		break;//Quit inner loop early
	    }
	    cur.bitmap &= ~(1U << bit);
	}
	prev_count = cur.count;
	//Prevent re-load of bmc from moving up
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	__atomic_load(&ht->buckets[bix].bmc, &cur, __ATOMIC_ACQUIRE);
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
    index_t start = ring_mod(hash, ht->ncells);
    index_t idx = start;
    do
    {
	if (__atomic_load_n(&ht->cellar[idx].elem, __ATOMIC_RELAXED) == elem)
	{
	    void *old = elem;
	    if (__atomic_compare_exchange_n(&ht->cellar[idx].elem,
					    &old,
					    NULL,
					    /*weak=*/false,
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
    bool success = remove_bkt_by_ptr(ht, elem, ring_mod(hash, ht->nbkts));
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
		  index_t bix,
		  p64_hazardptr_t *hazpp)
{
    uint32_t prev_count;
    struct bmc cur;
    __atomic_load(&ht->buckets[bix].bmc, &cur, __ATOMIC_ACQUIRE);
    do
    {
	if (cur.bitmap == 0)
	{
	    break;
	}
	struct bmc old = cur;
	while (cur.bitmap != 0)
	{
	    //Bitmap indicates which buckets in neighbourhood that contain
	    //elements which hash this bucket
	    uint32_t bit = __builtin_ctz(cur.bitmap);
	    index_t idx = (bix + bit) % ht->nbkts;
	    void *elem = atomic_load_acquire(&ht->buckets[idx].elem,
					     hazpp,
					     ht->use_hp);
	    if (elem != NULL && ht->cf(elem, key) == 0)
	    {
		//Found our element
		//Clear bit in bix bitmap
		struct bmc new;
		new.bitmap = old.bitmap & ~(1U << bit);
		new.count = old.count + 1;
		if (__atomic_compare_exchange(&ht->buckets[bix].bmc,
					      &old,
					      &new,
					      /*weak=*/false,
					      __ATOMIC_RELAXED,
					      __ATOMIC_RELAXED))
		{
		    //Successfully updated bitmap, now clear bucket
		    __atomic_store_n(&ht->buckets[idx].elem,
				     NULL,
				     __ATOMIC_RELAXED);
		    return elem;
		}
		//Else bitmap changed
		break;//Quit inner loop early
	    }
	    cur.bitmap &= ~(1U << bit);
	}
	prev_count = cur.count;
	//Prevent re-load of bmc from moving up
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	__atomic_load(&ht->buckets[bix].bmc, &cur, __ATOMIC_ACQUIRE);
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
    index_t start = ring_mod(hash, ht->ncells);
    index_t idx = start;
    do
    {
	if (__atomic_load_n(&ht->cellar[idx].hash, __ATOMIC_ACQUIRE) == hash)
	{
	    void *elem = atomic_load_acquire(&ht->cellar[idx].elem,
					     hazpp,
					     ht->use_hp);
	    if (ht->cf(key, elem) == 0)
	    {
		if (__atomic_compare_exchange_n(&ht->cellar[idx].elem,
						&elem,
						NULL,
						/*weak=*/false,
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
    void *elem = remove_bkt_by_key(ht, key, ring_mod(hash, ht->nbkts), hazpp);
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
