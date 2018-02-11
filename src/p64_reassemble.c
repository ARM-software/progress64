//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "p64_reassemble.h"
#include "build_config.h"

#include "common.h"
#include "lockfree.h"

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
	       (seg->hashval < frag->hashval ||
	       (seg->hashval == frag->hashval &&
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
	assert(frag->hashval < frag->nextfrag->hashval ||
	       (frag->hashval == frag->nextfrag->hashval &&
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
dg_is_complete(p64_fragment_t **prev)
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
	if (frag->nextfrag == NULL || frag->nextfrag->hashval != frag->hashval)
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
		   frag->nextfrag->hashval == frag->hashval);
	    //TODO same hashval check whole key
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
	//Find first fragment of next datagram (as identified by hashval)
	uint32_t hashval = frag->hashval;
	while (frag->nextfrag != NULL && frag->nextfrag->hashval == hashval)
	{
	    frag = frag->nextfrag;
	}
	assert(frag->nextfrag == NULL || frag->nextfrag->hashval != hashval);
	prev = &frag->nextfrag;
	goto restart;
    }
    return NULL;
}

union fraglist
{
    struct
    {
	uint32_t earliest;
	unsigned accsize:14;
	unsigned totsize:14;
	p64_fragment_t *head; //A list of related fragments awaiting reassembly
    } st;
    __int128 ui;
} ALIGNED(sizeof(__int128));

struct p64_reassemble
{
    p64_reassemble_cb complete_cb;
    p64_reassemble_cb stale_cb;
    void *arg;
    uint32_t nentries;
    union fraglist fragtbl[0] ALIGNED(CACHE_LINE);
};

p64_reassemble_t *
p64_reassemble_alloc(uint32_t nentries,
		     p64_reassemble_cb complete_cb,
		     p64_reassemble_cb stale_cb,
		     void *arg)
{
    if (nentries < 1)
    {
        fprintf(stderr, "Invalid fragment table size %u\n", nentries), abort();
    }
    size_t nbytes = ROUNDUP(sizeof(p64_reassemble_t) +
			    nentries * sizeof(union fraglist), CACHE_LINE);
    p64_reassemble_t *fl = aligned_alloc(CACHE_LINE, nbytes);
    if (fl != NULL)
    {
	fl->complete_cb = complete_cb;
	fl->stale_cb = stale_cb;
	fl->arg = arg;
	fl->nentries = nentries;
	for (uint32_t i = 0; i < nentries; i++)
	{
	    fl->fragtbl[i].st.earliest = 0;//Not used for null fraglists
	    fl->fragtbl[i].st.totsize = OCT_SIZEMAX;
	    fl->fragtbl[i].st.accsize = 0U;
	    fl->fragtbl[i].st.head = NULL;
	}

	return fl;
    }
    return NULL;
}

void
p64_reassemble_free(p64_reassemble_t *fl)
{
    if (fl != NULL)
    {
	for (uint32_t i = 0; i < fl->nentries; i++)
	{
	    if (fl->fragtbl[i].st.head != NULL)
	    {
		fl->stale_cb(fl->arg, fl->fragtbl[i].st.head);
	    }
	}
	free(fl);
    }
}

static uint32_t
reassemble_dg(p64_reassemble_t *fl, p64_fragment_t **head)
{
    uint32_t numdg = 0;
    while (*head != NULL)
    {
	p64_fragment_t *dg = dg_is_complete(head);
	if (dg == NULL)
	{
	    break;
	}
	fl->complete_cb(fl->arg, dg);
	numdg++;
    }
    return numdg;
}

static inline uint32_t
min(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

static inline uint32_t
min_earliest(uint32_t a, uint32_t b, uint32_t now)
{
    return min((int32_t)(a - now), (int32_t)(b - now)) + now;
}

void
p64_reassemble_insert(p64_reassemble_t *re,
		      p64_fragment_t *frag)
{
    union fraglist *fl = &re->fragtbl[frag->hashval % re->nentries];

    //Initialise some local state
    uint16_t fragsize = LEN2OCT(frag->len);
    uint16_t totsize = TOTSIZE_OCT(frag);
    uint32_t earliest = frag->arrival;
    uint32_t now = earliest;
    bool false_positive = false;

    //Where to insert existing fragment list
    p64_fragment_t **last = &frag->nextfrag;
    frag->nextfrag = NULL;

    union fraglist old, neu;
restart:
    old.ui = fl->ui;
    if (old.st.head != NULL)
    {
	false_positive = false;
    }
    //Merge lists of fragments, insert previous head fragment at end of new list
    *last = old.st.head;
    neu.st.head = frag;
    //Use min to implement saturating add, don't overflow the allocated bits
    neu.st.accsize = min(OCT_SIZEMAX, old.st.accsize + fragsize);
    //Replace previous totsize if smaller
    neu.st.totsize = min(old.st.totsize, totsize);
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
	if (!lockfree_compare_exchange_16(&fl->ui,
					  &old.ui,
					  neu.ui,
					  /*weak=*/false,
					  __ATOMIC_RELEASE,
					  __ATOMIC_RELAXED))
	{
	    //CAS failed, restart from beginning
	    goto restart;
	}
	//Fragment(s) inserted
    }
    else
    {
	//We seem to have all fragments
	//Write a null element to the fraglist slot
	neu.st.head = NULL;
	neu.st.accsize = 0;
	neu.st.totsize = OCT_SIZEMAX;
	neu.st.earliest = 0;
	if (!lockfree_compare_exchange_16(&fl->ui,
					  &old.ui,
					  neu.ui,
					  /*weak=*/false,
					  __ATOMIC_ACQUIRE,//due to old.st.head
					  __ATOMIC_RELAXED))
	{
	    //CAS failed, restart from beginning
	    goto restart;
	}

	//Sort the fragments
	p64_fragment_t *head = sort_frags(frag);//Includes old.st.head fraglist
	//Attempt to reassemble fragments into complete datagrams
	false_positive = reassemble_dg(re, &head) == 0;
	//Check if there are fragments left (for different datagram)
	if (head != NULL)
	{
	    assert(reassemble_dg(re, &head) == 0);
	    last = &head;
	    fragsize = 0;
	    totsize = OCT_SIZEMAX;
	    earliest = now;
	    //Find the last fragment in the list
	    //Compute accumulated size of fragments and expected total size
	    while (*last != NULL)
	    {
		fragsize = min(OCT_SIZEMAX, fragsize + LEN2OCT((*last)->len));
		totsize = min(totsize, TOTSIZE_OCT(*last));
		earliest = min_earliest(earliest, (*last)->arrival, now);
		last = &(*last)->nextfrag;
	    }
	    //frag is the first fragment in the list of remaining fragments
	    frag = head;
	    //Update fraglist again
	    goto restart;
	}
    }
}

void
p64_reassemble_expire(p64_reassemble_t *re,
		      uint32_t time)
{
}
