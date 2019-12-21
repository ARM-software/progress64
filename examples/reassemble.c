//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "p64_reassemble.h"
#include "p64_hazardptr.h"
#include "p64_qsbr.h"
#include "expect.h"

#define IP_FRAG_RESV 0x8000  //Reserved fragment flag
#define IP_FRAG_DONT 0x4000  //Don't fragment flag
#define IP_FRAG_MORE 0x2000  //More fragments following flag
#define IP_FRAG_MASK 0x1fff  //Mask for fragment offset bits

static p64_fragment_t *
alloc_frag(uint32_t hash,
	   uint32_t arrival,
	   uint32_t offset,
	   uint32_t len,
	   bool more)
{
    assert(offset % 8 == 0);
    assert(!more || len % 8 == 0);
    p64_fragment_t *frag = malloc(sizeof(p64_fragment_t));
    if (frag == NULL)
	perror("malloc"), exit(-1);
    frag->hash = hash;
    frag->arrival = arrival;
    frag->fraginfo = (more ? IP_FRAG_MORE : 0) | (offset / 8U);
    frag->len = len;
    return frag;
}

static void
free_frag(p64_fragment_t *frag)
{
    while (frag != NULL)
    {
	p64_fragment_t *nextfrag = frag->nextfrag;
	free(frag);
	frag = nextfrag;
    }
}

static uint32_t
length(p64_fragment_t *frag)
{
    uint32_t len = 0;
    while (frag != NULL)
    {
	len += frag->len;
	frag = frag->nextfrag;
    }
    return len;
}

static void
complete(void *arg, p64_fragment_t *frag)
{
    (void)arg;
    EXPECT(frag->nextfrag != NULL);
    p64_fragment_t *ff = frag->nextfrag;
    while (ff != NULL)
    {
	EXPECT(ff->hash == frag->hash);
	ff = ff->nextfrag;
    }
    printf("Reassembled datagram: hash %#"PRIx64" length %u\n",
	   frag->hash, length(frag));
    free_frag(frag);
}

static p64_fragment_t *lastfree = NULL;
static bool done = false;

static void
stale(void *arg, p64_fragment_t *frag)
{
    (void)arg;
    p64_fragment_t *org = frag;
    EXPECT(frag != NULL);
    while (frag != NULL)
    {
	printf("%s fragment: hash %#"PRIx64" arrival %u\n",
	       done ? "Freeing" : "Stale",
	       frag->hash, frag->arrival);
	frag = frag->nextfrag;
    }
    EXPECT(lastfree != org);
    lastfree = org;
    free_frag(org);
}

//Reassemble requires 1 hazard pointer per thread
#define NUM_HAZARD_POINTERS 1

int main(int argc, char *argv[])
{
    void *smr = NULL;
    uint32_t flags = 0;
    bool extend = false;
    bool use_hp = false;

    if (argc == 2 && argv[1][0] == '-')
    {
	if (strchr(argv[1], 'e') != NULL)
	{
	    flags |= P64_REASSEMBLE_F_EXT;
	    extend = true;
	}
	if (strchr(argv[1], 'h') != NULL)
	{
	    flags |= P64_REASSEMBLE_F_HP;
	    use_hp = true;
	}
    }

    if (extend)
    {
	printf("Perform fragment table extension\n");
	printf("Use %s for safe memory reclamation\n", use_hp ? "HP" : "QSBR");
	if (use_hp)
	{
	    smr = p64_hazptr_alloc(10, NUM_HAZARD_POINTERS);
	    EXPECT(smr != NULL);
	    p64_hazptr_register(smr);
	}
	else
	{
	    smr = p64_qsbr_alloc(10);
	    EXPECT(smr != NULL);
	    p64_qsbr_register(smr);
	}
    }

    p64_reassemble_t *re = p64_reassemble_alloc(16, complete, stale,
						NULL, NULL, flags);
    EXPECT(re != NULL);

    p64_fragment_t *f1 = alloc_frag(0x01010101, 100, 0, 1504, true);
    EXPECT(f1 != NULL);
    p64_reassemble_insert(re, f1);
    if (extend)
    {
	EXPECT(p64_reassemble_extend(re) == true);
    }
    p64_fragment_t *f2 = alloc_frag(0x73737373, 101, 1504, 100, false);
    EXPECT(f2 != NULL);
    p64_reassemble_insert(re, f2);
    if (extend)
    {
	EXPECT(p64_reassemble_extend(re) == true);
    }
    p64_fragment_t *f3 = alloc_frag(0x01010101, 102, 1504, 100, false);
    EXPECT(f3 != NULL);
    EXPECT(lastfree == NULL);
    p64_reassemble_insert(re, f3);
    if (extend)
    {
	EXPECT(p64_reassemble_extend(re) == true);
    }
    EXPECT(lastfree == NULL);
    p64_fragment_t *f4 = alloc_frag(0x01010101, 102, 0, 1504, true);
    EXPECT(f4 != NULL);
    p64_reassemble_insert(re, f4);
    if (extend)
    {
	EXPECT(p64_reassemble_extend(re) == true);
    }
    p64_reassemble_expire(re, 102);
    EXPECT(lastfree == f2);
    done = true;
    p64_reassemble_free(re);
    EXPECT(lastfree == f4);

    if (extend)
    {
	if (use_hp)
	{
	    p64_hazptr_dump(stdout);
	    EXPECT(p64_hazptr_reclaim() == 0);
	    p64_hazptr_unregister();
	    p64_hazptr_free(smr);
	}
	else
	{
	    EXPECT(p64_qsbr_reclaim() == 0);
	    p64_qsbr_unregister();
	    p64_qsbr_free(smr);
	}
    }

    printf("reassemble test complete\n");
    return 0;
}
