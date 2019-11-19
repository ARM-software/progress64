//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "p64_hazardptr.h"
#include "p64_hashtable.h"
#include "expect.h"

//Hashtable requires 2 hazard pointers per thread
#define NUM_HAZARD_POINTERS 2

static p64_hashvalue_t
hash(uint32_t k)
{
    return k;
}

struct my_elem
{
    p64_hashelem_t next;
    p64_hashvalue_t hash;
    uint32_t key;
};

static struct my_elem *
he_alloc(uint32_t k)
{
    struct my_elem *he = malloc(sizeof(struct my_elem));
    if (he == NULL)
	perror("malloc"), exit(-1);
    he->next.hash = 0xDEADBABE;
    he->next.next = NULL;
    he->hash = hash(k);
    he->key = k;
    return he;
}

struct state
{
    size_t nelems;
    size_t prev_idx;
};

static void
print_cb(void *arg,
	 p64_hashelem_t *he,
	 size_t idx)
{
    struct state *st = arg;
    st->nelems++;
    struct my_elem *me = (struct my_elem *)he;
    if (idx != st->prev_idx)
    {
	if (st->prev_idx != ~(size_t)0)
	{
	    printf("\n");
	}
	st->prev_idx = idx;
	//Assume 4 elements per bucket
	printf("%zu.%zu:", idx / 4, idx % 4);
    }
    printf(" <h=%"PRIxPTR",k=%"PRIu32">", me->hash, me->key);
}

static size_t
traverse(p64_hashtable_t *ht)
{
    struct state st = { 0, ~(size_t)0 };
    p64_hashtable_traverse(ht, print_cb, &st);
    printf("\n");
    return st.nelems;
}

static int
compf(const p64_hashelem_t *he,
      const void *key)
{
    uint32_t k = *(const uint32_t*)key;
    struct my_elem *m = (struct my_elem *)he;
    return m->key < k ? -1 : m->key > k ? 1 : 0;
}

int main(void)
{
    p64_hpdomain_t *hpd = p64_hazptr_alloc(10, NUM_HAZARD_POINTERS);
    EXPECT(hpd != NULL);
    p64_hazptr_register(hpd);

    p64_hashtable_t *ht = p64_hashtable_alloc(1, compf, P64_HASHTAB_F_HP);
    EXPECT(ht != NULL);
    traverse(ht);

    struct my_elem *h1 = he_alloc(1);
    p64_hashtable_insert(ht, &h1->next, h1->hash);
    EXPECT(traverse(ht) == 1);
    struct my_elem *h2 = he_alloc(2);
    p64_hashtable_insert(ht, &h2->next, h2->hash);
    EXPECT(traverse(ht) == 2);
    struct my_elem *h3 = he_alloc(3);
    p64_hashtable_insert(ht, &h3->next, h3->hash);
    EXPECT(traverse(ht) == 3);
    struct my_elem *h4 = he_alloc(4);
    p64_hashtable_insert(ht, &h4->next, h4->hash);
    EXPECT(traverse(ht) == 4);
    struct my_elem *h5 = he_alloc(5);
    p64_hashtable_insert(ht, &h5->next, h5->hash);
    EXPECT(traverse(ht) == 5);
    struct my_elem *h9 = he_alloc(9);
    p64_hashtable_insert(ht, &h9->next, h9->hash);
    EXPECT(traverse(ht) == 6);

    p64_hazardptr_t hp = P64_HAZARDPTR_NULL;
    struct my_elem *me = (struct my_elem *)p64_hashtable_lookup(ht, &(uint32_t){2}, hash(2), &hp);
    EXPECT(me != NULL);
    if (me != NULL)
    {
	printf("Found key %u node %p hazp %p (%p)\n", me->key, me, hp, *hp);
	p64_hazptr_release_ro(&hp);
	assert(hp == P64_HAZARDPTR_NULL);
    }
    else
    {
	printf("Key %u not found\n", 2);
	assert(hp == P64_HAZARDPTR_NULL);
    }
    printf("p64_hazptr_num_free()=%u\n", p64_hazptr_dump(stdout));

    me = (struct my_elem *)p64_hashtable_lookup(ht, &(uint32_t){8}, hash(8), &hp);
    EXPECT(me == NULL);
    if (me != NULL)
    {
	printf("Found key %u node %p hazp %p (%p)\n", me->key, me, hp, *hp);
	p64_hazptr_release_ro(&hp);
	assert(hp == P64_HAZARDPTR_NULL);
    }
    else
    {
	printf("Key %u not found\n", 8);
	assert(hp == P64_HAZARDPTR_NULL);
    }
    printf("p64_hazptr_num_free()=%u\n", p64_hazptr_dump(stdout));

    me = (struct my_elem *)p64_hashtable_lookup(ht, &(uint32_t){9}, hash(9), &hp);
    EXPECT(me != NULL);
    if (me != NULL)
    {
	printf("Found key %u node %p hazp %p (%p)\n", me->key, me, hp, *hp);
	p64_hazptr_dump(stdout);
	p64_hazptr_release_ro(&hp);
	assert(hp == P64_HAZARDPTR_NULL);
    }
    else
    {
	printf("Key %u not found\n", 9);
	assert(hp == P64_HAZARDPTR_NULL);
    }
    printf("p64_hazptr_num_free()=%u\n", p64_hazptr_dump(stdout));

    printf("Remove 2\n");
    EXPECT(p64_hashtable_remove(ht, &h2->next, hash(2)));
    EXPECT(traverse(ht) == 5);
    printf("Remove 1\n");
    EXPECT(p64_hashtable_remove(ht, &h1->next, hash(1)));
    EXPECT(traverse(ht) == 4);
    printf("Remove 3\n");
    EXPECT(p64_hashtable_remove(ht, &h3->next, hash(3)));
    EXPECT(traverse(ht) == 3);
    printf("Remove 9\n");
    EXPECT(p64_hashtable_remove(ht, &h9->next, hash(9)));
    EXPECT(traverse(ht) == 2);
    EXPECT(p64_hashtable_remove_by_key(ht, &(uint32_t){4}, hash(4), &hp) == (p64_hashelem_t *)h4);
    EXPECT(p64_hashtable_remove_by_key(ht, &(uint32_t){5}, hash(5), &hp) == (p64_hashelem_t *)h5);
    p64_hazptr_release_ro(&hp);
    p64_hashtable_free(ht);
    printf("p64_hazptr_num_free()=%u\n", p64_hazptr_dump(stdout));
    free(h1);
    free(h2);
    free(h3);
    free(h4);
    free(h5);
    free(h9);

    p64_hazptr_unregister();
    p64_hazptr_free(hpd);

    printf("hashtable test complete\n");
    return 0;
}
