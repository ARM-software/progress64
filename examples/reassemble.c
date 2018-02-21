//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "p64_reassemble.h"
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
    EXPECT(frag->nextfrag != NULL);
    p64_fragment_t *ff = frag->nextfrag;
    while (ff != NULL)
    {
	EXPECT(ff->hash == frag->hash);
	ff = ff->nextfrag;
    }
    printf("Reassembled datagram: hash %"PRIu64" length %u\n",
	   frag->hash, length(frag));
    free_frag(frag);
}

static p64_fragment_t *lastfree = NULL;
static bool done = false;

static void
stale(void *arg, p64_fragment_t *frag)
{
    p64_fragment_t *org = frag;
    EXPECT(frag != NULL);
    while (frag != NULL)
    {
	printf("%s fragment: hash %"PRIu64" arrival %u\n",
	       done ? "Freeing" : "Stale",
	       frag->hash, frag->arrival);
	frag = frag->nextfrag;
    }
    EXPECT(lastfree != org);
    lastfree = org;
    free_frag(org);
}

int main(void)
{
    p64_reassemble_t *re = p64_reassemble_alloc(15, complete, stale, NULL);
    EXPECT(re != NULL);

    p64_fragment_t *f1 = alloc_frag(1, 100, 0, 1504, true);
    EXPECT(f1 != NULL);
    p64_reassemble_insert(re, f1);
    p64_fragment_t *f2 = alloc_frag(16, 101, 1504, 100, false);
    EXPECT(f2 != NULL);
    p64_reassemble_insert(re, f2);
    p64_fragment_t *f3 = alloc_frag(1, 102, 1504, 100, false);
    EXPECT(f3 != NULL);
    EXPECT(lastfree == NULL);
    p64_reassemble_insert(re, f3);
    EXPECT(lastfree == NULL);
    p64_fragment_t *f4 = alloc_frag(1, 102, 0, 1504, true);
    EXPECT(f4 != NULL);
    p64_reassemble_insert(re, f4);
    p64_reassemble_expire(re, 102);
    EXPECT(lastfree == f2);
    done = true;
    p64_reassemble_free(re);
    EXPECT(lastfree == f4);

    printf("reassemble test complete\n");
    return 0;
}
