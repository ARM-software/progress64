//Copyright (c) 2020, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <assert.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "p64_cuckooht.h"
#include "p64_hazardptr.h"
#include "p64_qsbr.h"
#include "build_config.h"

#include "common.h"
#include "os_abstraction.h"
#include "lockfree.h"
#include "arch.h"
#include "err_hnd.h"

//Some magic to define CRC32C intrinsics
#ifdef __aarch64__
#ifndef __ARM_FEATURE_CRC32
#pragma GCC target("+crc")
#endif
#include <arm_acle.h>
#define CRC32C(x, y) __crc32cw((x), (y))
#endif
#if defined __x86_64__
#ifndef __SSE4_2__
#pragma GCC target("sse4.2")
#endif
#include <x86intrin.h>
//x86 crc32 intrinsics seem to compute CRC32C (not CRC32)
#define CRC32C(x, y) __crc32d((x), (y))
#endif
#ifdef __arm__
//ARMv7 does not have a CRC instruction
//Use a pseudo RNG instead
static inline uint32_t
xorshift32(uint32_t x)
{
    /* Algorithm "xor" from p. 4 of Marsaglia, "Xorshift RNGs" */
    //x == 0 will return 0
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}
#define CRC32C(x, y) xorshift32((y))
#endif

//Signatures are truncated hashes used for fast matching
typedef uint16_t sig_t;
#define SIZEOF_SIG 2

//Number of slots per bucket (cache line)
#define BKT_SIZE ((CACHE_LINE - __SIZEOF_INT__) / (__SIZEOF_POINTER__ + SIZEOF_SIG))
#if BKT_SIZE > 8
#undef BKT_SIZE
#define BKT_SIZE 8
#endif

#if defined __aarch64__ || defined __arm__
#ifndef __ARM_NEON
#pragma GCC target("+simd")
#endif
#include <arm_neon.h>
#else
#undef __ARM_NEON
#endif

//A shorter synonym for __atomic_compare_exchange_n()
#define atomic_cmpxchg(p, o, n, s, f) \
    __atomic_compare_exchange_n((p), (o), (n), /*weak=*/false, (s), (f))

//Slot is destination for a move-in-progress
#define TAG_DST (uintptr_t)1
#define HAS_DST(ptr) (((uintptr_t)(ptr) & TAG_DST) != 0)
#define SET_DST(ptr) (void *)((uintptr_t)(ptr) |  TAG_DST)

//Slot is source for a move-in-progress
#define TAG_SRC (uintptr_t)2
#define HAS_SRC(ptr) (((uintptr_t)(ptr) & TAG_SRC) != 0)
#define SET_SRC(ptr) (void *)((uintptr_t)(ptr) |  TAG_SRC)

//Index into source/destination bucket
#define BITS_IDX (uintptr_t)(7 << 2)
#define GET_IDX(ptr)      (((uintptr_t)(ptr) & BITS_IDX) >> 1)
#define SET_IDX(ptr, idx) (void *)((uintptr_t)(ptr) | ((idx) << 1))

//All tag bits
#define BITS_ALL (TAG_DST | TAG_SRC | BITS_IDX)
#define CLR_ALL(ptr) (void *)((uintptr_t)(ptr) & ~BITS_ALL)
#define HAS_ANY(ptr) (((uintptr_t)(ptr) & BITS_ALL) != 0)

//Indicates elements hashing to this bucket exists in cellar
#define CELLAR_BIT 1
//Change count increment (leave lsb 0 for cellar bit)
#define CHGCNT_INC 2

//Logical implication: if 'p' is true then 'q' must also be true
#define IMPLIES(p, q) (!(p) || (q))

struct cell
{
    p64_cuckooelem_t *elem;
    p64_cuckoohash_t hash;
} ALIGNED(sizeof(ptrpair_t));

union cellpp
{
    struct cell cell;
    ptrpair_t pp;
};

struct bucket
{
    uint32_t chgcnt;
    sig_t sigs[BKT_SIZE];//Truncated hashes
    //Elems last so that we can SIMD-read beyond the end of sigs[]
    p64_cuckooelem_t *elems[BKT_SIZE];
} ALIGNED(CACHE_LINE);

static_assert(sizeof(struct bucket) == CACHE_LINE, "sizeof(struct bucket) == CACHE_LINE");

//Index into bucket array
typedef uint32_t bix_t;

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
ring_mod(p64_cuckoohash_t hash, bix_t modulo)
{
    bix_t rem = hash % modulo;
    assert(rem < modulo);
    return rem;
}

static inline p64_cuckooelem_t *
atomic_load_acquire(p64_cuckooelem_t **pptr,
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

struct p64_cuckooht
{
    p64_cuckooht_compare cf;
    bix_t nbkts;
    bix_t ncells;
    uint8_t use_hp;//Use hazard pointers for safe memory reclamation
    struct cell *cellar;//Pointer to cell array
    struct bucket buckets[] ALIGNED(CACHE_LINE);
    //Cell array follows the last bucket
};

void
p64_cuckooht_traverse(p64_cuckooht_t *ht,
		      p64_cuckooht_trav_cb cb,
		      void *arg)
{
    p64_hazardptr_t hp = P64_HAZARDPTR_NULL;
    for (bix_t idx = 0; idx < ht->nbkts; idx++)
    {
	for (uint32_t j = 0; j < BKT_SIZE; j++)
	{
	    void *elem = atomic_load_acquire(&ht->buckets[idx].elems[j],
					     &hp,
					     ~BITS_ALL,
					     ht->use_hp);
	    elem = CLR_ALL(elem);
	    if (elem != NULL)
	    {
		if (!ht->use_hp)
		{
		    p64_qsbr_acquire();
		    cb(arg, elem, idx * BKT_SIZE + j);
		    p64_qsbr_release();
		}
		else
		{
		    cb(arg, elem, idx);
		}
	    }
	}
    }
    for (bix_t idx = 0; idx < ht->ncells; idx++)
    {
	void *elem = atomic_load_acquire(&ht->cellar[idx].elem,
					 &hp,
					 ~BITS_ALL,
					 ht->use_hp);
	if (elem != NULL)
	{
	    if (!ht->use_hp)
	    {
		p64_qsbr_acquire();
		cb(arg, elem, idx | (1U << 31));
		p64_qsbr_release();
	    }
	    else
	    {
		cb(arg, elem, idx | CELLAR_BIT);
	    }
	}
    }
    atomic_ptr_release(&hp, ht->use_hp);
}

void
p64_cuckooht_check(p64_cuckooht_t *ht)
{
    bix_t histo[BKT_SIZE + 1] = { 0 };//Zero-init whole array
    bix_t ncellbits = 0;
    size_t nelems = 0;
    for (bix_t bix = 0; bix < ht->nbkts; bix++)
    {
	uint32_t ne = 0;
	for (uint32_t j = 0; j < BKT_SIZE; j++)
	{
	    ne += ht->buckets[bix].elems[j] != NULL;
	}
	histo[ne]++;
	nelems += ne;
	if (ht->buckets[bix].chgcnt & CELLAR_BIT)
	{
	    ncellbits++;
	}
    }
    size_t ncellar = 0;
    for (bix_t i = 0; i < ht->ncells; i++)
    {
	nelems += (ht->cellar[i].elem != NULL);
	ncellar += (ht->cellar[i].elem != NULL);
    }
    assert(ncellbits <= ncellar);
    assert(IMPLIES(ncellbits == 0, ncellar == 0));
    printf("Cuckoo hash table: %u buckets @ %u slots, %u cells, "
	    "%zu elements, load=%.2f\n",
	    ht->nbkts, BKT_SIZE, ht->ncells,
	    nelems, (float)nelems / (ht->nbkts * BKT_SIZE + ht->ncells));
    printf("Bucket occupancy histogram\n");
    for (uint32_t i = 0; i < BKT_SIZE + 1; i++)
    {
	printf("%u: %7u (%.3f)\n", i, histo[i], histo[i] / (float)ht->nbkts);
    }
    printf("Cellar: %zu\n", ncellar);
}

#define VALID_FLAGS P64_CUCKOOHT_F_HP

p64_cuckooht_t *
p64_cuckooht_alloc(size_t nelems,
		   size_t ncells,
		   p64_cuckooht_compare cf,
		   uint32_t flags)
{
#if __SIZEOF_POINTER__ == 8
    //Ensure 32-bit bix_t handles all elements
    if (UNLIKELY(nelems == 0 || nelems >= ((uint64_t)BKT_SIZE << 32)))
#else
    if (UNLIKELY(nelems == 0))
#endif
    {
	report_error("cuckooht", "invalid number of elements", nelems);
	return NULL;
    }
#if __SIZEOF_POINTER__ == 8
    if (UNLIKELY(ncells >= (UINT64_C(1) << 32)))
    {
	report_error("cuckooht", "invalid number of elements", ncells);
	return NULL;
    }
#endif
    if (UNLIKELY((flags & ~VALID_FLAGS) != 0))
    {
	report_error("cuckooht", "invalid flags", flags);
	return NULL;
    }
    size_t nbkts = (nelems + BKT_SIZE - 1) / BKT_SIZE;
    //Must have at least two buckets
    if (nbkts < 2)
    {
	nbkts = 2;
    }
    size_t sz = sizeof(p64_cuckooht_t) +
		sizeof(struct bucket) * nbkts +
		sizeof(struct cell) * ncells;
    p64_cuckooht_t *ht = p64_malloc(sz, CACHE_LINE);
    if (ht != NULL)
    {
	memset(ht, 0, sz);
	//All buckets cleared (NULL element pointers)
	//All cells cleared (NULL element pointers)
	ht->cf = cf;
	ht->nbkts = nbkts;
	ht->ncells = ncells;
	ht->use_hp = (flags & P64_CUCKOOHT_F_HP) != 0;
	ht->cellar = (struct cell *)&ht->buckets[nbkts];
    }
    return ht;
}

#undef VALID_FLAGS

void
p64_cuckooht_free(p64_cuckooht_t *ht)
{
    if (ht != NULL)
    {
	//Check if hash table is empty
	for (bix_t i = 0; i < ht->nbkts; i++)
	{
	    for (uint32_t j = 0; j < BKT_SIZE; j++)
	    {
		//No need to use HP or QSBR, elements are not accessed
		if (ht->buckets[i].elems[j] != NULL)
		{
		    report_error("cuckooht", "hash table not empty", 0);
		    return;
		}
	    }
	}
	for (bix_t i = 0; i < ht->ncells; i++)
	{
	    //No need to use HP or QSBR, elements are not accessed
	    if (ht->cellar[i].elem != NULL)
	    {
		report_error("cuckooht", "hash table not empty", 0);
		return;
	    }
	}
	p64_mfree(ht);
    }
}

#if defined __arm__
#if BKT_SIZE == 4
typedef uint32_t mask_t;
#define MASK_SHIFT 3 //8 bits per boolean flag
#define MASK_ONE 0xFFU
#elif BKT_SIZE == 8
typedef uint64_t mask_t;
#define MASK_ONE 0xFFULL
#define MASK_SHIFT 3 //8 bits per boolean flag
#endif
#elif defined __aarch64__ && (BKT_SIZE == 6 || BKT_SIZE == 8)
typedef uint64_t mask_t;
#define MASK_SHIFT 3 //8 bits per boolean flag
#define MASK_ONE 0xFFUL
#else
typedef uint32_t mask_t;
#define MASK_SHIFT 0 //1 bit per boolean flag
#define MASK_ONE 1U
#endif

UNROLL_LOOPS ALWAYS_INLINE
static inline mask_t
find_matches(const sig_t sigs[BKT_SIZE], sig_t sig)
{
    mask_t matches;
#if defined __arm__
    static_assert(BKT_SIZE == 4 || BKT_SIZE == 8, "BKT_SIZE == 4 || BKT_SIZE == 8");
    uint16x8_t vsig = vdupq_n_u16(sig);
    uint16x8_t vsigs = vld1q_u16(sigs);
    //compare sigs: equality => ~0 (per lane), inequality => 0
    uint16x8_t vmatch16 = vceqq_u16(vsigs, vsig);
    uint8x8_t vmatch8 = vmovn_u16(vmatch16);
    uint64x1_t vmatch = vreinterpret_u64_u8(vmatch8);
    //Cast will mask off upper bits which were based on non-existent sigs[4..7]
    matches = (mask_t)vget_lane_u64(vmatch, 0);
#elif defined __aarch64__
    static_assert(BKT_SIZE == 6 || BKT_SIZE == 8, "BKT_SIZE == 6 || BKT_SIZE == 8");
    uint16x8_t vsig = vdupq_n_u16(sig);
    uint16x8_t vsigs = vld1q_u16(sigs);
    //compare sigs: equality => ~0 (per lane), inequality => 0
    uint16x8_t vmatch16 = vceqq_u16(vsigs, vsig);
    uint8x8_t vmatch8 = vmovn_u16(vmatch16);
    uint64x1_t vmatch = vreinterpret_u64_u8(vmatch8);
    matches = vget_lane_u64(vmatch, 0);
#if BKT_SIZE == 6
    //Mask off upper 16 bits which were based on non-existent sigs[6..7]
    matches &= ((1UL << 48) - 1);
#endif
#else
    matches = 0;
    for (uint32_t i = 0; i < BKT_SIZE; i++)
    {
	if (sigs[i] == sig)
	{
	    matches |= MASK_ONE << (i << MASK_SHIFT);
	}
    }
#endif
    return matches;
}

ALWAYS_INLINE
static inline p64_cuckooelem_t *
check_matches(p64_cuckooht_t *ht,
	      struct bucket *bkt,
	      mask_t mask,
	      const void *key,
	      p64_cuckoohash_t hash,
	      p64_hazardptr_t *hazpp)
{
    assert(mask != 0);
    //Do full checks for all matches one by one
    do
    {
	uint32_t i = __builtin_ctzll(mask) >> MASK_SHIFT;
	p64_cuckooelem_t *elem =
	    atomic_load_acquire(&bkt->elems[i],
				hazpp,
				~BITS_ALL,
				ht->use_hp);
	elem = CLR_ALL(elem);
	if (elem != NULL && elem->hash == hash && ht->cf(elem, key) == 0)
	{
	    //Found our element
	    return elem;
	}
	//Else false positive signature match
	mask &= ~(MASK_ONE << (i << MASK_SHIFT));
    }
    while (mask != 0);
    return NULL;
}

NO_INLINE
static p64_cuckooelem_t *
search_cellar(p64_cuckooht_t *ht,
	      const void *key,
	      p64_cuckoohash_t hash,
	      p64_hazardptr_t *hazpp)
{
    if (UNLIKELY(ht->ncells == 0))
    {
	return NULL;
    }
    bix_t start = ring_mod(hash, ht->ncells);
    //Start search from bucket-specific position
    bix_t idx = start;
    do
    {
	if (__atomic_load_n(&ht->cellar[idx].hash, __ATOMIC_RELAXED) == hash)
	{
	    p64_cuckooelem_t *elem =
		atomic_load_acquire(&ht->cellar[idx].elem,
				    hazpp,
				    ~BITS_ALL,
				    hazpp != NULL && ht->use_hp);
	    if (elem != NULL && ht->cf(elem, key) == 0)
	    {
		//Found our element
		return elem;
	    }
	}
	idx = ring_add(idx, 1, ht->ncells);
    }
    while (idx != start);
    return NULL;
}

p64_cuckooelem_t *
p64_cuckooht_lookup(p64_cuckooht_t *ht,
		    const void *key,
		    p64_cuckoohash_t hash,
		    p64_hazardptr_t *hazpp)
{
    //Caller must call QSBR acquire/release/quiescent as appropriate
    bix_t bix0 = ring_mod(hash, ht->nbkts);
    struct bucket *bkt0 = &ht->buckets[bix0];
    PREFETCH_FOR_READ(bkt0);
    bix_t bix1 = ring_mod(CRC32C(0, hash), ht->nbkts);
    if (UNLIKELY(bix1 == bix0))
    {
	bix1 = ring_add(bix1, 1, ht->nbkts);
    }
    struct bucket *bkt1 = &ht->buckets[bix1];
    PREFETCH_FOR_READ(bkt1);
    uint32_t chgcnt;
    p64_cuckooelem_t *elem;
    do
    {
	chgcnt = __atomic_load_n(&bkt0->chgcnt, __ATOMIC_ACQUIRE);
	//Create bit masks with all matching hashes
	mask_t mask0 = find_matches(bkt0->sigs, hash);
	if (mask0 != 0)
	{
	    //Perform complete checks for any matches
	    elem = check_matches(ht, bkt0, mask0, key, hash, hazpp);
	    if (elem != NULL)
	    {
		return elem;
	    }
	}
	mask_t mask1 = find_matches(bkt1->sigs, hash);
	if (mask1 != 0)
	{
	    elem = check_matches(ht, bkt1, mask1, key, hash, hazpp);
	    if (elem != NULL)
	    {
		return elem;
	    }
	}
	//Re-read the change counter too see if we might have missed
	//an element which was moved between the buckets
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
    }
    while (__atomic_load_n(&bkt0->chgcnt, __ATOMIC_RELAXED) != chgcnt);
    //Check cellar bit if any elements which hash to this bucket are
    //present in the cellar
    if ((chgcnt & CELLAR_BIT) != 0)
    {
	elem = search_cellar(ht, key, hash, hazpp);
	if (elem != NULL)
	{
	    return elem;
	}
    }
    return NULL;
}

//Write signature to bucket
static inline void
write_sig(struct bucket *bkt,
	  uint32_t idx,
	  sig_t oldsig,
	  p64_cuckooelem_t *elem,
	  sig_t newsig)
{
    for (;;)
    {
	//Try to swing (update) sig field
	if (atomic_cmpxchg(&bkt->sigs[idx],
			   &oldsig,
			   newsig,
			   __ATOMIC_RELEASE,
			   __ATOMIC_RELAXED))
	{
	    //Success
	    return;
	}
	//Else failed to update sig
	//Some other thread has written sig and possibly also elem fields
	//after our write to elem
	//Check if our element is still present
	smp_fence(StoreLoad);
	oldsig = __atomic_load_n(&bkt->sigs[idx], __ATOMIC_RELAXED);
	if (__atomic_load_n(&bkt->elems[idx], __ATOMIC_RELAXED) != elem)
	{
	    //No, element not present anymore, don't write sig
	    return;
	}
	//Else our element is still present so continue trying to update
	//sig field
    }
}

//Insert *new* element into bucket
static inline bool
bucket_insert(struct bucket *bkt,
	      uint32_t mask,
	      p64_cuckooelem_t *elem,
	      p64_cuckoohash_t hash)
{
    //Try all matches one by one
    while (mask != 0)
    {
	uint32_t i = __builtin_ctz(mask);
	sig_t oldsig = __atomic_load_n(&bkt->sigs[i], __ATOMIC_RELAXED);
	p64_cuckooelem_t *old = NULL;
	//Try to grab slot
	if (atomic_cmpxchg(&bkt->elems[i],
			   &old,
			   elem,
			   __ATOMIC_RELEASE,
			   __ATOMIC_RELAXED))
	{
	    //Success, the slot is ours
	    //Now update corresponding signature field
	    write_sig(bkt, i, oldsig, elem, hash);
	    return true;
	}
	mask &= ~(1U << i);
    }
    return false;
}

//Compute the sibling bucket index
static inline bix_t
sibling_bix(p64_cuckooht_t *ht,
	    p64_cuckoohash_t hash,
	    bix_t bix)
{
    bix_t sib_bix;
    bix_t bix0 = ring_mod(hash, ht->nbkts);
    if (bix0 != bix)
    {
	sib_bix = bix0;//bix0 is sibling bucket index
    }
    else
    {
	bix_t bix1 = ring_mod(CRC32C(0, hash), ht->nbkts);
	if (UNLIKELY(bix1 == bix0))
	{
	    bix1 = ring_add(bix1, 1, ht->nbkts);
	}
	sib_bix = bix1;
    }
    return sib_bix;
}

//Clean tags from destination slot
static void
clean_dst(p64_cuckooht_t *ht,
	  p64_cuckooelem_t *elem,
	  bix_t dst_bix,
	  uint32_t dst_idx,
	  uint32_t src_idx)
{
    assert(!HAS_ANY(elem));
    struct bucket *dst_bkt = &ht->buckets[dst_bix];
    p64_cuckooelem_t *old = SET_IDX(SET_SRC(elem), src_idx);
    //Remove the tags, keeping the bare element pointer
    if (atomic_cmpxchg(&dst_bkt->elems[dst_idx],
		       &old,
		       elem,
		       __ATOMIC_RELAXED,
		       __ATOMIC_RELAXED))
    {
	//Tags removed, element is clean, move complete
#if 0
	bix_t src_bix = sibling_bix(ht, elem->hash, dst_bix);
	printf("Move %u:%u -> %u:%u complete\n",
		src_bix, src_idx,
		dst_bix, dst_idx);
#endif
    }
    //Else destination slot already updated
}

//Clear source slot
static void
clear_src(p64_cuckooht_t *ht,
	  p64_cuckooelem_t *elem,
	  bix_t src_bix,
	  uint32_t src_idx,
	  bix_t dst_bix,
	  uint32_t dst_idx)
{
    assert(!HAS_ANY(elem));
    struct bucket *src_bkt = &ht->buckets[src_bix];
    p64_cuckooelem_t *old = SET_IDX(SET_DST(elem), dst_idx);
    //A pre-check of the elem field, we don't want to unnecessarily increment
    //the change counter
    if (__atomic_load_n(&src_bkt->elems[src_idx], __ATOMIC_RELAXED) == old)
    {
	//First we increment the change counter to indicate the (imminent)
	//disappearance of an element
	//We always increment the change counter of the primary bucket,
	//regardless which is the source or destination bucket
	//This allows lookup (and remove) to only read and check the change
	//counter of one bucket instead of both buckets, this seems to save
	//some nanoseconds in the lookup
	bix_t bix = ring_mod(elem->hash, ht->nbkts);
	__atomic_fetch_add(&ht->buckets[bix].chgcnt,
			   CHGCNT_INC, __ATOMIC_RELAXED);
	//Then we actually disappear the element
	if (atomic_cmpxchg(&src_bkt->elems[src_idx],
			   &old,
			   NULL,
			   __ATOMIC_RELEASE,
			   __ATOMIC_RELAXED))
	{
	    //Source slot cleared
	}
	//Else source slot already updated
    }
    //Else source slot already updated
    //Now clean element pointer in destination slot
    clean_dst(ht, elem, dst_bix, dst_idx, src_idx);
}

//Move existing element from source to destination bucket
static void
move_elem(p64_cuckooht_t *ht,
	  p64_cuckooelem_t *elem,
	  bix_t src_bix,
	  uint32_t src_idx,
	  bix_t dst_bix,
	  uint32_t dst_idx)
{
    assert(!HAS_ANY(elem));
    //Write element to destination slot together with info about source slot
    struct bucket *dst_bkt = &ht->buckets[dst_bix];
    sig_t oldsig = __atomic_load_n(&dst_bkt->sigs[dst_idx], __ATOMIC_RELAXED);
    p64_cuckooelem_t *old = SET_DST(NULL);
    if (atomic_cmpxchg(&dst_bkt->elems[dst_idx],
		       &old,
		       SET_IDX(SET_SRC(elem), src_idx),
		       __ATOMIC_RELEASE,//Keep load(oldsig) above
		       __ATOMIC_RELAXED))
    {
	//Success
	//Now update corresponding hash field
	write_sig(dst_bkt, dst_idx, oldsig, elem, elem->hash);
    }
    //Else destination slot already updated
    //Now clear source slot
    clear_src(ht, elem, src_bix, src_idx, dst_bix, dst_idx);
}

static void
help_move(p64_cuckooht_t *ht,
	  p64_cuckooelem_t *elem,
	  bix_t bix0,
	  uint32_t idx0)
{
    assert(HAS_ANY(elem) != 0);
    assert(idx0 < BKT_SIZE);
    bix_t bix1 = sibling_bix(ht, elem->hash, bix0);
    uint32_t idx1 = GET_IDX(elem);
    assert(idx1 < BKT_SIZE);
    bix_t src_bix, dst_bix;
    uint32_t src_idx, dst_idx;
    if (HAS_DST(elem))
    {
	src_bix = bix0;
	src_idx = idx0;
	dst_bix = bix1;
	dst_idx = idx1;
    }
    else if (HAS_SRC(elem))
    {
	src_bix = bix1;
	src_idx = idx1;
	dst_bix = bix0;
	dst_idx = idx0;
    }
    else
    {
	abort();
    }
    move_elem(ht, CLR_ALL(elem), src_bix, src_idx, dst_bix, dst_idx);
}

//Find and reserve empty slot in destination bucket, return index
static bool
find_empty(p64_cuckooht_t *ht,
	   bix_t dst_bix,
	   uint32_t *dst_idx)
{
    //Check destination bucket for empty slots
    struct bucket *bkt = &ht->buckets[dst_bix];
    for (uint32_t i = 0; i < BKT_SIZE; i++)
    {
	if (bkt->elems[i] == NULL)
	{
	    //Found empty slot, try to reserve it
	    p64_cuckooelem_t *old = NULL;
	    if (atomic_cmpxchg(&bkt->elems[i],
			       &old,
			       SET_DST(NULL),//Reserved slot
			       __ATOMIC_RELAXED,
			       __ATOMIC_RELAXED))
	    {
		//Success reserving empty slot
		*dst_idx = i;
		return true;
	    }
	    //Else slot not empty anymore
	}
	//Else slot not empty
    }
    //No empty slot in destination bucket
    return false;
}

//Attempt to move one of the elements in the source bucket to its sibling
//(destination) bucket, thus freeing up a slot in this bucket
static bool
make_room(p64_cuckooht_t *ht,
	  bix_t src_bix)
{
    p64_hazardptr_t hp = P64_HAZARDPTR_NULL;
    struct bucket *bkt = &ht->buckets[src_bix];
    for (uint32_t src_idx = 0; src_idx < BKT_SIZE; src_idx++)
    {
	p64_cuckooelem_t *elem = atomic_load_acquire(&bkt->elems[src_idx],
						     &hp,
						     ~BITS_ALL,
						     ht->use_hp);
	if (UNLIKELY(elem == NULL))
	{
	    //Slot unexpectedly became empty (great!)
	    return true;
	}
	else if (UNLIKELY(HAS_ANY(elem)))
	{
	    //Slot is source or destination of move-in-progress
	    if (CLR_ALL(elem) != NULL)
	    {
		help_move(ht, elem, src_bix, src_idx);
	    }
	    //Else slot is reserved but we don't know anything else
	    continue;
	}
	//Else found 'clean' element
	//Find and reserve empty slot in sibling (destination) bucket
	bix_t dst_bix = sibling_bix(ht, elem->hash, src_bix);
	uint32_t dst_idx;
	if (find_empty(ht, dst_bix, &dst_idx))
	{
	    //Tag source element with index in destination bucket
	    //Destination bucket itself can be computed from hash
	    if (atomic_cmpxchg(&bkt->elems[src_idx],
			       &elem,
			       SET_IDX(SET_DST(elem), dst_idx),
			       __ATOMIC_RELAXED,
			       __ATOMIC_RELAXED))
	    {
		//Move started!
		//Let's try to complete the move ourselves
		move_elem(ht, elem, src_bix, src_idx, dst_bix, dst_idx);
		return true;
	    }
	    //Else slot changed
	    //Undo reservation
	    struct bucket *dst_bkt = &ht->buckets[dst_bix];
	    p64_cuckooelem_t *old = SET_DST(NULL);
	    if (!atomic_cmpxchg(&dst_bkt->elems[dst_idx],
				&old,
				NULL,
				__ATOMIC_RELAXED,
				__ATOMIC_RELAXED))
	    {
		//TODO can this really happen?
		fprintf(stderr, "Failed to clear reservation\n");
		fflush(stderr);
		abort();
	    }
	    //Empty slot is clean again
	}
	//Else no empty slot in sibling bucket
    }
    //No element in source bucket can be moved
    atomic_ptr_release(&hp, ht->use_hp);
    return false;
}

NO_INLINE
static bool
insert_cell(p64_cuckooht_t *ht,
	    p64_cuckooelem_t *elem,
	    p64_cuckoohash_t hash,
	    struct bucket *bkt)
{
    if (UNLIKELY(ht->ncells == 0))
    {
	return false;
    }
    bix_t start = ring_mod(hash, ht->ncells);
    bix_t idx = start;
    do
    {
	if (__atomic_load_n(&ht->cellar[idx].elem, __ATOMIC_RELAXED) == NULL)
	{
	    //Write elem & hash fields atomically
	    p64_cuckoohash_t oldhash =
		__atomic_load_n(&ht->cellar[idx].hash, __ATOMIC_RELAXED);
	    union cellpp old = { .cell.elem = NULL, .cell.hash = oldhash };
	    union cellpp new = { .cell.elem = elem, .cell.hash = hash };
	    if (lockfree_compare_exchange_pp((ptrpair_t *)&ht->cellar[idx],
					     &old.pp,
					     new.pp,
					    /*weak=*/false,
					    __ATOMIC_RELEASE,
					    __ATOMIC_RELAXED))
	    {
		//Set cellar bit in first bucket to indicate presence of
		//element in cellar
		uint32_t old, new;
		do
		{
		    old = __atomic_load_n(&bkt->chgcnt, __ATOMIC_RELAXED);
		    new = (old + CHGCNT_INC) | CELLAR_BIT;
		}
		while (!atomic_cmpxchg(&bkt->chgcnt,
				       &old,
				       new,
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

UNROLL_LOOPS
bool
p64_cuckooht_insert(p64_cuckooht_t *ht,
		    p64_cuckooelem_t *elem,
		    p64_cuckoohash_t hash)
{
    if (UNLIKELY(HAS_ANY(elem)))
    {
	report_error("cuckooht", "element has low bits set", elem);
	return false;
    }
    elem->hash = hash;
    if (!ht->use_hp)
    {
	p64_qsbr_acquire();
    }
    bix_t bix0 = ring_mod(hash, ht->nbkts);
    bix_t bix1 = ring_mod(CRC32C(0, hash), ht->nbkts);
    if (UNLIKELY(bix1 == bix0))
    {
	bix1 = ring_add(bix1, 1, ht->nbkts);
    }
    struct bucket *bkt0 = &ht->buckets[bix0];
    struct bucket *bkt1 = &ht->buckets[bix1];
    bool success;
    for (;;)
    {
	//Compute emptiness of the two buckets
	uint32_t empty0 = 0, empty1 = 0;
	for (uint32_t i = 0; i < BKT_SIZE; i++)
	{
	    if (bkt0->elems[i] == NULL)
	    {
		empty0 |= 1U << i;
	    }
	    if (bkt1->elems[i] == NULL)
	    {
		empty1 |= 1U << i;
	    }
	}
	//Inserting in the least full bucket helps increasing the load factor
	uint32_t numempt0 = __builtin_popcount(empty0);
	uint32_t numempt1 = __builtin_popcount(empty1);
	success = bucket_insert(numempt0 > numempt1 ? bkt0 : bkt1,
				numempt0 > numempt1 ? empty0 : empty1,
				elem, hash);
	if (success)
	{
	    break;
	}
	success = bucket_insert(numempt0 > numempt1 ? bkt1 : bkt0,
				numempt0 > numempt1 ? empty1 : empty0,
				elem, hash);
	if (success)
	{
	    break;
	}
	if (make_room(ht, bix0))
	{
	    continue;
	}
	if (make_room(ht, bix1))
	{
	    continue;
	}
	//Could not make room in any of the buckets
	//Try to insert in cellar
	success = insert_cell(ht, elem, hash, bkt0);
	break;
    }
    if (!ht->use_hp)
    {
	p64_qsbr_release();
    }
    return success;
}

ALWAYS_INLINE
static inline bool
bucket_remove(p64_cuckooht_t *ht,
	      bix_t bix,
	      p64_cuckooelem_t *elem,
	      uint32_t mask)
{
    struct bucket *bkt = &ht->buckets[bix];
    p64_hazardptr_t hp = P64_HAZARDPTR_NULL;
    do
    {
	p64_cuckooelem_t *old;
	uint32_t i = __builtin_ctz(mask);
	for (;;)
	{
	    old = atomic_load_acquire(&bkt->elems[i],
				      &hp, ~BITS_ALL, ht->use_hp);
	    if (LIKELY(!HAS_ANY(old)))
	    {
		//Slot contains clean element
		break;
	    }
	    //Slot contains element tagged with move-in-progress
	    //Must help complete the move
	    help_move(ht, CLR_ALL(old), bix, i);
	}
	atomic_ptr_release(&hp, ht->use_hp);
	//Replace clean element with NULL
	sig_t oldsig = __atomic_load_n(&bkt->sigs[i], __ATOMIC_RELAXED);
	old = elem;
	if (atomic_cmpxchg(&bkt->elems[i],
			   &old,
			   NULL,
			   __ATOMIC_RELEASE,
			   __ATOMIC_RELAXED))
	{
	    //Write invalid signature value
	    write_sig(bkt, i, oldsig, NULL, elem->hash);
	    return true;
	}
	mask &= ~(1U << i);
    }
    while (mask != 0);
    return false;
}

static void
update_cellar(p64_cuckooht_t *ht,
	      bix_t bix)
{
    uint32_t old, new;
    do
    {
	old  = __atomic_load_n(&ht->buckets[bix].chgcnt, __ATOMIC_ACQUIRE);
	new = old;
	new &= ~CELLAR_BIT;
	for (bix_t i = 0; i < ht->ncells; i++)
	{
	    p64_cuckoohash_t hash =
		__atomic_load_n(&ht->cellar[i].hash, __ATOMIC_RELAXED);
	    p64_cuckooelem_t *elem =
		__atomic_load_n(&ht->cellar[i].elem, __ATOMIC_RELAXED);
	    if (elem != NULL && ring_mod(hash, ht->nbkts) == bix)
	    {
		//Found another element which hashes to same bix
		new |= CELLAR_BIT;
		break;
	    }
	}
	if (new == old)
	{
	    //No need to update
	    break;
	}
	new += CHGCNT_INC;
	//Attempt to update chgcnt, fail if count has changed
    }
    while (!atomic_cmpxchg(&ht->buckets[bix].chgcnt,
			   &old,
			   new,
			   __ATOMIC_RELEASE,
			   __ATOMIC_RELAXED));
}

NO_INLINE
static bool
remove_cell_by_ptr(p64_cuckooht_t *ht,
		   p64_cuckooelem_t *elem,
		   p64_cuckoohash_t hash)
{
    if (UNLIKELY(ht->ncells == 0))
    {
	return NULL;
    }
    bix_t start = ring_mod(hash, ht->ncells);
    bix_t idx = start;
    do
    {
	if (__atomic_load_n(&ht->cellar[idx].elem, __ATOMIC_RELAXED) == elem)
	{
	    //Write elem & hash fields atomically
	    union cellpp old = { .cell.elem = elem, .cell.hash = hash };
	    union cellpp nul = { .cell.elem = NULL, .cell.hash = ~hash };
	    if (lockfree_compare_exchange_pp((ptrpair_t *)&ht->cellar[idx],
					     &old.pp,
					     nul.pp,
					    /*weak=*/false,
					    __ATOMIC_RELAXED,
					    __ATOMIC_RELAXED))
	    {
		update_cellar(ht, ring_mod(hash, ht->nbkts));
		return true;
	    }
	}
	idx = ring_add(idx, 1, ht->ncells);
    }
    while (idx != start);
    return false;
}

UNROLL_LOOPS
bool
p64_cuckooht_remove(p64_cuckooht_t *ht,
		    p64_cuckooelem_t *elem,
		    p64_cuckoohash_t hash)
{
    if (UNLIKELY(HAS_ANY(elem)))
    {
	report_error("cuckooht", "element has low bits set", elem);
	return false;
    }
    if (!ht->use_hp)
    {
	p64_qsbr_acquire();
    }
    bix_t bix0 = ring_mod(hash, ht->nbkts);
    struct bucket *bkt0 = &ht->buckets[bix0];
    PREFETCH_FOR_READ(bkt0);
    bix_t bix1 = ring_mod(CRC32C(0, hash), ht->nbkts);
    if (UNLIKELY(bix1 == bix0))
    {
	bix1 = ring_add(bix1, 1, ht->nbkts);
    }
    struct bucket *bkt1 = &ht->buckets[bix1];
    PREFETCH_FOR_READ(bkt1);
    uint32_t chgcnt;
    bool success = false;
    do
    {
	chgcnt = __atomic_load_n(&bkt0->chgcnt, __ATOMIC_ACQUIRE);
	uint32_t mask0 = 0, mask1 = 0;
	for (uint32_t i = 0; i < BKT_SIZE; i++)
	{
	    if (CLR_ALL(bkt0->elems[i]) == elem)
	    {
		mask0 |= 1U << i;
	    }
	    if (CLR_ALL(bkt1->elems[i]) == elem)
	    {
		mask1 |= 1U << i;
	    }
	}
	if (mask0 != 0)
	{
	    success = bucket_remove(ht, bix0, elem, mask0);
	    if (success)
	    {
		goto done;
	    }
	}
	if (mask1 != 0)
	{
	    success = bucket_remove(ht, bix1, elem, mask1);
	    if (success)
	    {
		goto done;
	    }
	}
	//Re-read the change counter too see if we might have missed
	//an element which was moved between the buckets
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
    }
    while (__atomic_load_n(&bkt0->chgcnt, __ATOMIC_RELAXED) != chgcnt);
    if ((chgcnt & CELLAR_BIT) != 0)
    {
	success = remove_cell_by_ptr(ht, elem, hash);
    }
done:
    if (!ht->use_hp)
    {
	p64_qsbr_release();
    }
    return success;
}
