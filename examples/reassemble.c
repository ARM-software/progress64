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
    p64_hpdomain_t *hpd = NULL;
    bool extend = false;
    if (argc == 2 && strcmp(argv[1], "-e") == 0)
    {
	printf("Will do reassembly table extension\n");
	extend = true;
    }

    if (extend)
    {
	hpd = p64_hazptr_alloc(10, NUM_HAZARD_POINTERS);
	EXPECT(hpd != NULL);
	p64_hazptr_register(hpd);
    }

    p64_reassemble_t *re = p64_reassemble_alloc(16, complete, stale,
						NULL, NULL, extend);
    EXPECT(re != NULL);

    p64_fragment_t *f1 = alloc_frag(0x01010101, 100, 0, 1504, true);
    EXPECT(f1 != NULL);
    p64_reassemble_insert(re, f1);
    EXPECT(p64_reassemble_extend(re) == extend);
    p64_fragment_t *f2 = alloc_frag(0x73737373, 101, 1504, 100, false);
    EXPECT(f2 != NULL);
    p64_reassemble_insert(re, f2);
    EXPECT(p64_reassemble_extend(re) == extend);
    p64_fragment_t *f3 = alloc_frag(0x01010101, 102, 1504, 100, false);
    EXPECT(f3 != NULL);
    EXPECT(lastfree == NULL);
    p64_reassemble_insert(re, f3);
    EXPECT(p64_reassemble_extend(re) == extend);
    EXPECT(lastfree == NULL);
    p64_fragment_t *f4 = alloc_frag(0x01010101, 102, 0, 1504, true);
    EXPECT(f4 != NULL);
    p64_reassemble_insert(re, f4);
    EXPECT(p64_reassemble_extend(re) == extend);
    p64_reassemble_expire(re, 102);
    EXPECT(lastfree == f2);
    done = true;
    p64_reassemble_free(re);
    EXPECT(lastfree == f4);

    if (extend)
    {
	p64_hazptr_dump(stdout);
	EXPECT(p64_hazptr_reclaim() == 0);
	p64_hazptr_unregister();
	p64_hazptr_free(hpd);
    }

    printf("reassemble test complete\n");
    return 0;
}
