//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "p64_hazardptr.h"
#include "p64_qsbr.h"
#include "p64_spinlock.h"
#include "p64_reassemble.h"
#include "build_config.h"
#include "os_abstraction.h"

#include "common.h"
#include "atomic.h"
#include "arch.h"
#include "err_hnd.h"

//totsize=65535 => totsize_oct=8192 => 14 bits required
#define OCT_SIZEMAX ((1U << 14U) - 1U)

//IPv4 fragment info
#define IP_FRAG_RESV 0x8000U  //Reserved fragment flag
#define IP_FRAG_DONT 0x4000U  //Don't fragment flag
#define IP_FRAG_MORE 0x2000U  //More fragments following flag
#define IP_FRAG_MASK 0x1FFFU  //Mask for fragment offset bits

#define FI2OFF(fi) (((fi) & IP_FRAG_MASK) * 8U)
#define FI2MORE(fi) (((fi) & IP_FRAG_MORE) != 0)
#define LEN2OCT(l) (((l) + 7U) / 8U)

static inline uint32_t
TOTSIZE_OCT(p64_fragment_t *f)
{
    if (FI2MORE(f->fraginfo))
    {
	//Not last fragment, use largest possible size
	return OCT_SIZEMAX;
    }
    else
    {
	//Last fragment, compute total size of reassembled datagram
	return (FI2OFF(f->fraginfo) + f->len + 7U) / 8U;
    }
}

#ifndef NDEBUG
static uint32_t
count_frags(p64_fragment_t *frag)
{
    uint32_t num = 0;
    while (frag != NULL)
    {
	num++;
	frag = frag->nextfrag;
    }
    return num;
}
#endif

static p64_fragment_t *
sort_frags(p64_fragment_t *frag)
{
    p64_fragment_t *head = NULL;
#ifndef NDEBUG
    uint32_t numfrags = count_frags(frag);
#endif
    //Add fragments one by one creating a sorted list
    //TODO check/optimise for already sorted list?
    while (frag != NULL)
    {
	//Save nextfrag for next iteration
	p64_fragment_t *nextfrag = frag->nextfrag;
	//Traverse the list looking for the right position
	p64_fragment_t **prev = &head;
	p64_fragment_t *seg = head;
	while (seg != NULL &&
	       (seg->hash < frag->hash ||
	       (seg->hash == frag->hash &&
		FI2OFF(seg->fraginfo) < FI2OFF(frag->fraginfo))))
	{
	    prev = &seg->nextfrag;
	    seg = seg->nextfrag;
	}
	*prev = frag;
	frag->nextfrag = seg;
	//Continue with next fragment
	frag = nextfrag;
    }
#ifndef NDEBUG
    //Verify correctness of list insertion (internal consistency check)
    frag = head;
    while (frag->nextfrag != NULL)
    {
	assert(frag->hash < frag->nextfrag->hash ||
	       (frag->hash == frag->nextfrag->hash &&
		FI2OFF(frag->fraginfo) <= FI2OFF(frag->nextfrag->fraginfo)));
	frag = frag->nextfrag;
    }
#endif
    assert(count_frags(head) == numfrags);
    return head;
}

//Check if datagram is complete
//If true, return ptr to first fragment in list else return NULL
static p64_fragment_t *
is_complete(p64_fragment_t **prev)
{
restart: (void)0;
    p64_fragment_t *frag = *prev;
    uint32_t expected_off = 0;
    while (frag != NULL)
    {
	if (FI2OFF(frag->fraginfo) != expected_off)
	{
	    //Missing fragment
	    return NULL;
	}
	if (frag->nextfrag == NULL || frag->nextfrag->hash != frag->hash)
	{
	    //Last segment should have MORE flag cleared
	    if (FI2MORE(frag->fraginfo))
	    {
		//MORE flag set on last segment, missing true last segment
		break;
	    }
	    //Last fragment for datagram
	    //It has the MORE flag set
	    //Snip and return this fragment list for a complete datagram
	    p64_fragment_t *head = *prev;
	    *prev = frag->nextfrag;
	    frag->nextfrag = NULL;
	    return head;
	}
	else //Not last fragment of this datagram
	{
	    assert(frag->nextfrag != NULL &&
		   frag->nextfrag->hash == frag->hash);
	    //TODO same hash, check whole key
	    //Non-last fragment should have MORE flag set
	    //TODO beware of duplicated last fragment
	    //TODO check MORE flag after de-duplication?
	    if (!FI2MORE(frag->fraginfo))
	    {
		//Premature MORE flag (duplicate last fragment?)
		break;
	    }

	    if (FI2OFF(frag->nextfrag->fraginfo) >
		FI2OFF(frag->fraginfo) + frag->len)
	    {
		//Hole between frag and frag->nextfrag
		break;
	    }

	    if (FI2OFF(frag->nextfrag->fraginfo) <
		FI2OFF(frag->fraginfo) + frag->len)
	    {
		//Overlap between frag and frag->nextfrag
		//This is not our problem, caller must handle
	    }
	}
	expected_off += frag->len;
	frag = frag->nextfrag;
    }
    if (frag != NULL)
    {
	//There is a discontinuity between frag and frag->nextfrag
	//Find first fragment of next datagram (as identified by hash)
	uint32_t hash = frag->hash;
	while (frag->nextfrag != NULL && frag->nextfrag->hash == hash)
	{
	    frag = frag->nextfrag;
	}
	assert(frag->nextfrag == NULL || frag->nextfrag->hash != hash);
	prev = &frag->nextfrag;
	goto restart;
    }
    return NULL;
}

struct fraglist
{
    union
    {
	struct
	{
	    uint32_t earliest;
	    unsigned accsize:14;
	    unsigned totsize:14;
	    unsigned aba:3;
	    unsigned closed:1;
	};
	uint64_t ui64;
    };
    p64_fragment_t *head; //A list of related fragments awaiting reassembly
} ALIGNED(2 * sizeof(uint64_t));

#define FL_NULL (struct fraglist) { .earliest = 0, .accsize = 0, .totsize = OCT_SIZEMAX, .aba = 0, .closed = 0, .head = NULL }
#define FL_NULL_CLOSED (struct fraglist) { .earliest = 0, .accsize = 0, .totsize = OCT_SIZEMAX, .aba = 0, .closed = 1, .head = NULL }

struct fragtbl
{
    union idx_size
    {
	struct
	{
#if __SIZEOF_POINTER__ == 8
	uint32_t idx;
	uint32_t shift;//size = 1U << (32 - shift)
#elif __SIZEOF_POINTER__ == 4
	//Match size of 32-bit pointer
	unsigned idx:24;
	unsigned shift:8;//size = 1U << (32 - shift)
#else
#error
#endif
	};
	uintptr_t ui;
    } i_s;
    struct fraglist *base;
};

struct p64_reassemble
{
    struct fragtbl ft[2];//Current and next (if any) fragment tables
    uint32_t cur;//Index of current fragment table
    uint8_t extendable;
    uint8_t use_hp;
    p64_reassemble_cb complete_cb;
    void *complete_arg;
    p64_reassemble_cb stale_cb;
    void *stale_arg;
    p64_spinlock_t lock;//Mutex for p64_reassemble_extend()
};

#define SHIFT_TO_SIZE(sht) (1U << (32 - (sht)))
#define SIZE_TO_SHIFT(sz) (32 - __builtin_ctz((sz)))

//Perform atomic read of specified fragtable
static inline struct fragtbl
read_fragtbl(p64_reassemble_t *re, uint32_t idx, p64_hazardptr_t *hpp)
{
    if (LIKELY(!re->extendable))
    {
	assert(idx == 0);
	(void)idx;
	//Not extendable, just do relaxed non-atomic read of first element
	return re->ft[0];
    }
    union idx_size i_s;
    struct fraglist *fl;
    uint32_t i = idx % 2;
    do
    {
	if (UNLIKELY(!re->use_hp))
	{
	    fl = atomic_load_ptr(&re->ft[i].base, __ATOMIC_ACQUIRE);
	}
	else
	{
	    fl = p64_hazptr_acquire(&re->ft[i].base, hpp);
	}
	i_s.ui = atomic_load_n(&re->ft[i].i_s.ui, __ATOMIC_RELAXED);
	if (fl == NULL || i_s.idx != idx)
	{
	    //Slot cleared or overwritten with new fragment table
	    return (struct fragtbl) { i_s, NULL };
	}
	//Since reading base and idx_size is not atomic, re-read base to
	//verify it hasn't changed
	//The memory pointed by base cannot be reused since we have marked
	//it using a hazard pointer
	smp_fence(LoadLoad);
    }
    while (fl != atomic_load_ptr(&re->ft[i].base, __ATOMIC_RELAXED));
    return (struct fragtbl) { i_s, fl };
}

//Perform atomic write of specified fragtable
//Return previous value
static inline struct fragtbl
write_fragtbl(struct fragtbl *vec, uint32_t idx, struct fragtbl ft)
{
    union
    {
	__int128 i128;
	struct fragtbl ft;
    } old, neu;
    uint32_t i = idx % 2;
    neu.ft = ft;
    old.i128 = atomic_exchange_n((__int128 *)&vec[i], neu.i128, __ATOMIC_ACQ_REL);
    return old.ft;
}

#define VALID_FLAGS (P64_REASSEMBLE_F_HP | P64_REASSEMBLE_F_EXT)

p64_reassemble_t *
p64_reassemble_alloc(uint32_t size,
		     p64_reassemble_cb complete_cb,
		     p64_reassemble_cb stale_cb,
		     void *complete_arg,
		     void *stale_arg,
		     uint32_t flags)
{
    if (size < 1 || !IS_POWER_OF_TWO(size))
    {
	report_error("reassemble", "invalid fragment table size", size);
	return NULL;
    }
    if (UNLIKELY((flags & ~VALID_FLAGS) != 0))
    {
	report_error("reassemble", "invalid flags", flags);
	return NULL;
    }
    p64_reassemble_t *re = p64_malloc(sizeof(p64_reassemble_t), CACHE_LINE);
    if (re != NULL)
    {
	size_t nbytes = size * sizeof(struct fraglist);
	re->ft[0].i_s.idx = 0;
	re->ft[0].i_s.shift = SIZE_TO_SHIFT(size);
	assert(SHIFT_TO_SIZE(re->ft[0].i_s.shift) == size);
	re->ft[0].base = p64_malloc(nbytes, CACHE_LINE);
	if (re->ft[0].base != NULL)
	{
	    re->ft[1].i_s.idx = 0;
	    re->ft[1].i_s.shift = 0;
	    re->ft[1].base = NULL;
	    re->cur = 0;
	    re->extendable = (flags & P64_REASSEMBLE_F_EXT) != 0;
	    re->use_hp = (flags & P64_REASSEMBLE_F_HP) != 0;
	    re->complete_cb = complete_cb;
	    re->stale_cb = stale_cb;
	    re->complete_arg = complete_arg;
	    re->stale_arg = stale_arg;
	    p64_spinlock_init(&re->lock);
	    for (uint32_t i = 0; i < size; i++)
	    {
		re->ft[0].base[i] = FL_NULL;
	    }
	    return re;
	}
	p64_mfree(re);
    }
    return NULL;
}

#undef VALID_FLAGS

void
p64_reassemble_free(p64_reassemble_t *re)
{
    if (re != NULL)
    {
	struct fragtbl ft = re->ft[re->cur % 2];
	assert(ft.i_s.idx == re->cur);
	//Treat any remaining fragments in current table as stale
	uint32_t size = SHIFT_TO_SIZE(ft.i_s.shift);
	for (uint32_t i = 0; i < size; i++)
	{
	    if (ft.base[i].head != NULL)
	    {
		re->stale_cb(re->stale_arg, ft.base[i].head);
	    }
	}
	p64_mfree(ft.base);
	assert(re->ft[(re->cur + 1) % 2].base == NULL);
	p64_mfree(re);
    }
}

static uint32_t
reassemble(p64_reassemble_t *re,
	   p64_fragment_t **head)
{
    uint32_t numdg = 0;
    while (*head != NULL)
    {
	p64_fragment_t *dg = is_complete(head);
	if (dg == NULL)
	{
	    break;
	}
	re->complete_cb(re->complete_arg, dg);
	numdg++;
    }
    return numdg;
}

static inline uint32_t
umin(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

static inline int32_t
smin(int32_t a, int32_t b)
{
    return a < b ? a : b;
}

static inline uint32_t
min_earliest(uint32_t a, uint32_t b, uint32_t now)
{
    return smin((int32_t)(a - now), (int32_t)(b - now)) + now;
}

static inline p64_fragment_t **
recompute(p64_fragment_t **head,
	  uint16_t *fragsize,
	  uint16_t *totsize,
	  uint32_t *earliest,
	  uint32_t now)
{
    p64_fragment_t **last = head;
    *fragsize = 0;
    *totsize = OCT_SIZEMAX;
    *earliest = now;
    //Find the last fragment in the list
    //Compute accumulated size of fragments and expected total size
    while (*last != NULL)
    {
	*fragsize = umin(OCT_SIZEMAX, *fragsize + LEN2OCT((*last)->len));
	*totsize = umin(*totsize, TOTSIZE_OCT(*last));
	*earliest = min_earliest(*earliest, (*last)->arrival, now);
	last = &(*last)->nextfrag;
    }
    return last;
}

static p64_fragment_t *
insert_frags(p64_reassemble_t *re,
	     struct fraglist *fl,
	     p64_fragment_t *frag)
{
    //Prefetch-for-store before we read the line to update
    PREFETCH_FOR_WRITE(fl);
    uint32_t now = frag->arrival;
    bool false_positive = false;
    uint16_t fragsize;
    uint16_t totsize;
    uint32_t earliest;
    //Where to insert existing fragment list
    p64_fragment_t **last;
    last = recompute(&frag, &fragsize, &totsize, &earliest, now);

    union
    {
	struct fraglist st;
	__int128 ui128;
    } old, neu;
restart:
    old.st.ui64 = atomic_load_n(&fl->ui64, __ATOMIC_RELAXED);
    old.st.head = atomic_load_ptr(&fl->head, __ATOMIC_RELAXED);
    if (UNLIKELY(old.st.closed))
    {
	//Fraglist slot has been closed, caller must retry with next table
	return frag;
    }
    if (old.st.head != NULL)
    {
	false_positive = false;
    }
    //Merge lists of fragments, insert previous head fragment at end of new list
    *last = old.st.head;
    neu.st.head = frag;
    neu.st.aba = old.st.aba + 1;
    neu.st.closed = 0;
    //Use min to implement saturating add, don't overflow the allocated bits
    neu.st.accsize = umin(OCT_SIZEMAX, old.st.accsize + fragsize);
    //Replace previous totsize if smaller
    neu.st.totsize = umin(old.st.totsize, totsize);
    //Check if we have all fragments
    if (neu.st.accsize < neu.st.totsize || false_positive)
    {
	//Still missing fragment, write back updated fraglist
	if (old.st.head != NULL)
	{
	    neu.st.earliest = min_earliest(old.st.earliest, earliest, now);
	}
	else //old is null fraglist, earliest field not valid
	{
	    neu.st.earliest = earliest;
	}
	if (!atomic_compare_exchange_n((__int128 *)fl,
				       &old.ui128,
				       neu.ui128,
				       __ATOMIC_RELEASE,
				       __ATOMIC_RELAXED))
	{
	    //CAS failed, restart from beginning
	    PREFETCH_FOR_WRITE(fl);
	    goto restart;
	}
	//Fragment(s) inserted
    }
    else
    {
	//We seem to have all fragments
	//Write a null element to the fraglist slot
	neu.st = FL_NULL;
	if (!atomic_compare_exchange_n((__int128 *)fl,
				       &old.ui128,
				       neu.ui128,
				       __ATOMIC_ACQUIRE,//due to old.st.head
				       __ATOMIC_RELAXED))
	{
	    //CAS failed, restart from beginning
	    PREFETCH_FOR_WRITE(fl);
	    goto restart;
	}

	//Sort the fragments
	frag = sort_frags(frag);//Includes old.st.head fraglist
	//Attempt to reassemble fragments into complete datagrams
	false_positive = reassemble(re, &frag) == 0;
	//Check if there are fragments left (for different datagram)
	if (frag != NULL)
	{
	    assert(reassemble(re, &frag) == 0);
	    //Find the last fragment in the list
	    //Compute accumulated size of fragments and expected total size
	    last = recompute(&frag, &fragsize, &totsize, &earliest, now);
	    //Update fraglist again
	    PREFETCH_FOR_WRITE(fl);
	    goto restart;
	}
	//Else no fragments left, nothing to do
    }
    return NULL;
}

static void
split_and_insert_frags(p64_reassemble_t *re,
		       uint32_t cur,
		       struct fragtbl *ft,
		       p64_hazardptr_t *hpp,
		       p64_fragment_t *frag)
{
    while (frag != NULL)
    {
	//Find stretch of fragments with same hash
	p64_fragment_t **pnext = &frag->nextfrag;
	while (*pnext != NULL && (*pnext)->hash == frag->hash)
	{
	    pnext = &(*pnext)->nextfrag;
	}
	p64_fragment_t *next = *pnext;
	//Snip off list of fragments
	*pnext = NULL;
	for (;;)
	{
	    //Ensure we have a valid fragment table
	    while (ft->base == NULL)
	    {
		*ft = read_fragtbl(re, cur, hpp);
		if (ft->base != NULL)
		{
		    assert((uintptr_t)ft->base % sizeof(struct fraglist) == 0);
		    break;
		}
		//'cur' is stale, try next one
		cur++;
	    }
	    //Insert first stretch into fragment table
	    uint32_t idx = (uint32_t)frag->hash >> ft->i_s.shift;
	    assert(idx < SHIFT_TO_SIZE(ft->i_s.shift));
	    frag = insert_frags(re, &ft->base[idx], frag);
	    if (frag == NULL)
	    {
		//insert_frags() was successful, we are done
		break;
	    }
	    //Insertion failed, slot is closed
	    //Try next fragment table
	    cur++;
	    ft->base = NULL;
	}
	//Pointer to next stretch
	frag = next;
    }
}

void
p64_reassemble_insert(p64_reassemble_t *re,
		      p64_fragment_t *frag)
{
    p64_hazardptr_t hp = P64_HAZARDPTR_NULL;
    if (LIKELY(re->extendable && !re->use_hp))
    {
	p64_qsbr_acquire();
    }
    //Ensure single fragment is a proper list
    frag->nextfrag = NULL;
    //Get current fragment table
    uint32_t cur = atomic_load_n(&re->cur, __ATOMIC_ACQUIRE);
    //Insert fragment into current (or later) table
    struct fragtbl ft = { .base = NULL };
    split_and_insert_frags(re, cur, &ft, &hp, frag);
    if (re->extendable)
    {
	if (LIKELY(!re->use_hp))
	{
	    p64_qsbr_release();
	}
	else
	{
	    p64_hazptr_release(&hp);
	}
    }
}

static inline p64_fragment_t *
find_stale(p64_fragment_t **pfrag,
	   uint32_t time)
{
    p64_fragment_t *stale = NULL;
    while (*pfrag != NULL)
    {
	p64_fragment_t *frag = *pfrag;
	if ((int32_t)(frag->arrival - time) < 0)
	{
	    //Found stale fragment
	    //Remove it from linked list
	    *pfrag = frag->nextfrag;
	    //Insert it first in stale list
	    frag->nextfrag = stale;
	    stale = frag;
	    //We have already updated pfrag so continue directly to loop check
	    continue;
	}
	pfrag = &(*pfrag)->nextfrag;
    }
    return stale;
}

//Return true if slot was closed, otherwise false
static inline bool
expire_slot(p64_reassemble_t *re,
	    uint32_t cur,
	    struct fragtbl *ft,
	    p64_hazardptr_t *hpp,
	    struct fraglist *fl,
	    uint32_t time)
{
    bool closed = false;
    union
    {
	struct fraglist st;
	__int128 ui128;
    } old, neu;
    old.st.ui64 = atomic_load_n(&fl->ui64, __ATOMIC_RELAXED);
    old.st.head = atomic_load_ptr(&fl->head, __ATOMIC_RELAXED);
    do
    {
	if (old.st.head == NULL ||
	    (int32_t)(old.st.earliest - time) >= 0)
	{
	    //Null fraglist or no stale fragments
	    return false;
	}
	if (old.st.closed)
	{
	    //Found closed slot
	    return true;
	}
	//Found fraglist with at least one stale fragment
	//Swap in a null fraglist in its place
	neu.st = FL_NULL;
    }
    while (!atomic_compare_exchange_n((__int128 *)fl,
				      &old.ui128,//Updated on failure
				      neu.ui128,
				      __ATOMIC_ACQUIRE,
				      __ATOMIC_RELAXED));
    //CAS succeeded, we own the fraglist
    //Find the stale fragments
    p64_fragment_t *stale = find_stale(&old.st.head, time);
    if (old.st.head != NULL)
    {
	//Fresh fragments remain, insert back into table
	p64_fragment_t *frags = insert_frags(re, fl, old.st.head);
	if (UNLIKELY(frags != NULL))
	{
	    //Insertion failed, slot closed
	    closed = true;
	    //Insert fragments into next fragment table
	    split_and_insert_frags(re, cur + 1, ft, hpp, frags);
	}
    }
    if (stale != NULL)
    {
	//Return list with stale fragments to user
	re->stale_cb(re->stale_arg, stale);
    }
    return closed;
}

void
p64_reassemble_expire(p64_reassemble_t *re,
		      uint32_t time)
{
    p64_hazardptr_t hp = P64_HAZARDPTR_NULL;
    uint32_t cur;
    struct fragtbl ft;
    if (LIKELY(re->extendable && !re->use_hp))
    {
	p64_qsbr_acquire();
    }
    //Read the current fragment table
    do
    {
	cur = atomic_load_n(&re->cur, __ATOMIC_ACQUIRE);
	ft = read_fragtbl(re, cur, &hp);
    }
    while (ft.base == NULL);
    //Scan all slots and process any fraglist with expired fragment(s)
    //Scan from end, extend works from beginning
    for (uint32_t i = SHIFT_TO_SIZE(ft.i_s.shift) - 1; i != (uint32_t)-1; i--)
    {
	if (expire_slot(re, ft.i_s.idx, &ft, &hp, &ft.base[i], time))
	{
	    //Found closed slot
	    //Continue expiration in next fragment table
	    do
	    {
		ft = read_fragtbl(re, ++cur, &hp);
		//Next table is double the size so adjust array index
		i = 2 * i + 1;
	    }
	    while (ft.base == NULL);
	}
    }
    if (re->extendable)
    {
	if (LIKELY(!re->use_hp))
	{
	    p64_qsbr_release();
	}
	else
	{
	    p64_hazptr_release(&hp);
	}
    }
}

static void
migrate_slot(p64_reassemble_t *re,
	     struct fragtbl src,
	     struct fragtbl *dst,
	     p64_hazardptr_t *hpp,
	     uint32_t i)
{
    union
    {
	struct fraglist st;
	__int128 ui128;
    } old, neu;
    //Clear slots in destination table
    uint32_t factor = 1U << (src.i_s.shift - dst->i_s.shift);
    assert(factor ==
	   SHIFT_TO_SIZE(dst->i_s.shift) / SHIFT_TO_SIZE(src.i_s.shift));
    for (uint32_t j = 0; j < factor; j++)
    {
	dst->base[factor * i + j] = FL_NULL;
    }
    //Compute address of slot in source table
    struct fraglist *slot = &src.base[i];
    //Read fraglist from source slot
    //Non-atomic read, later CAS will verify atomicity
    old.st.head = atomic_load_ptr(&slot->head, __ATOMIC_ACQUIRE);
    old.st.ui64 = atomic_load_n(&slot->ui64, __ATOMIC_RELAXED);
    do
    {
	//Set closed bit in source slot using CAS, fail if slot has changed
	neu.st = FL_NULL_CLOSED;
    }
    while (!atomic_compare_exchange_n((__int128 *)slot,
				      &old.ui128,
				      neu.ui128,
				      __ATOMIC_ACQUIRE,
				      __ATOMIC_ACQUIRE));
    assert(old.st.closed == 0);
    if (old.st.head != NULL)
    {
	//Split and write fragments to destination table
	split_and_insert_frags(re, dst->i_s.idx, dst, hpp, old.st.head);
    }
}

bool
p64_reassemble_extend(p64_reassemble_t *re)
{
    if (UNLIKELY(!re->extendable))
    {
	report_error("reassemble", "Extend not supported", re);
	return false;
    }
    //Ensure mutual exclusion
    if (UNLIKELY(!p64_spinlock_try_acquire(&re->lock)))
    {
	return false;
    }
    bool success = false;
    //Read the current fragment table => 'old'
    uint32_t cur = atomic_load_n(&re->cur, __ATOMIC_ACQUIRE);
    struct fragtbl old = re->ft[cur % 2];
    if (LIKELY(old.i_s.shift != 0))
    {
	//Allocate a new fragment table with double the size
	uint32_t old_size = SHIFT_TO_SIZE(old.i_s.shift);
	uint32_t new_size = 2 * old_size;
	struct fraglist *base = p64_malloc(new_size * sizeof(struct fraglist),
					   CACHE_LINE);
	if (LIKELY(base != NULL))
	{
	    //Write new fragtable to next position so that threads can start to
	    //use it when their slot in the old fragtable has been closed
	    //The slots in the new fragtable are initially uninitialised
	    struct fragtbl neu = { { { cur + 1, SIZE_TO_SHIFT(new_size) } }, base };
	    assert(SHIFT_TO_SIZE(neu.i_s.shift) == new_size);
	    struct fragtbl prv = write_fragtbl(re->ft, cur + 1, neu);
	    //Verify that the fragtbl slot was unused
	    assert(prv.base == NULL);
	    //Migrate all fraglists from old to new table
	    //This will close slots in the old table and thus force threads to
	    //use the corresponding slots in the new table
	    p64_hazardptr_t hp = P64_HAZARDPTR_NULL;
	    for (uint32_t i = 0; i < old_size; i++)
	    {
		migrate_slot(re, old, &neu, &hp, i);
		assert(old.base[i].closed == 1);
	    }
	    if (UNLIKELY(re->use_hp))
	    {
		p64_hazptr_release(&hp);
	    }
	    //Make new table current
	    atomic_store_n(&re->cur, cur + 1, __ATOMIC_RELEASE);
	    //Remove old fragment table
	    prv = write_fragtbl(re->ft, cur, (struct fragtbl){ { { 0, 0} }, NULL});
	    assert(prv.base == old.base);
	    //Retire old fragtable, memory will be reclaimed when all threads
	    //have stopped referencing it
	    assert(prv.base != NULL);
	    if (UNLIKELY(re->use_hp))
	    {
		while (!p64_hazptr_retire(prv.base, p64_mfree))
		{
		    doze();
		}
	    }
	    else
	    {
		while (!p64_qsbr_retire(prv.base, p64_mfree))
		{
		    doze();
		}
	    }
	    success = true;
	}
	//Else out of memory
    }
    //Else already max size
    //Release and return
    p64_spinlock_release(&re->lock);
    return success;
}
