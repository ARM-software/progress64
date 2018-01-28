#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "p64_hashtable.h"

int hp_num_free(void);
void thread_state_init(int);
uint32_t hash_table_check(struct p64_hashtable *ht,
			  uint64_t (*f)(struct p64_hashentry *));


static p64_hashvalue_t hash(uint32_t k)
{
    return k;
}

struct my_entry
{
    struct p64_hashentry next;
    p64_hashvalue_t hash;
    uint32_t key;
};

static struct my_entry *he_alloc(uint32_t k)
{
    struct my_entry *he = malloc(sizeof(struct my_entry));
    if (he == NULL)
	perror("malloc"), exit(-1);
    he->next.hash = 0xDEADBABE;
    he->next.next = NULL;
    he->hash = hash(k);
    he->key = k;
    return he;
}

static uint64_t keyf(struct p64_hashentry *he)
{
    struct my_entry *me = (struct my_entry *)he;
    return me->key;
}

static int compf(const struct p64_hashentry *he, const void *key)
{
    uint32_t k = *(const uint32_t*)key;
    struct my_entry *m = (struct my_entry *)he;
    return m->key < k ? -1 : m->key > k ? 1 : 0;
}

int main()
{
    thread_state_init(0);
    struct p64_hashtable *ht = p64_hashtable_alloc(1);
    if (ht == NULL)
	perror("p64_hashtable_alloc"), exit(-1);
    hash_table_check(ht, keyf);

    struct my_entry *h1 = he_alloc(1);
    p64_hashtable_insert(ht, &h1->next, h1->hash);
    hash_table_check(ht, keyf);
    struct my_entry *h2 = he_alloc(2);
    p64_hashtable_insert(ht, &h2->next, h2->hash);
    hash_table_check(ht, keyf);
    struct my_entry *h3 = he_alloc(3);
    p64_hashtable_insert(ht, &h3->next, h3->hash);
    hash_table_check(ht, keyf);
    struct my_entry *h4 = he_alloc(4);
    p64_hashtable_insert(ht, &h4->next, h4->hash);
    hash_table_check(ht, keyf);
    struct my_entry *h5 = he_alloc(5);
    p64_hashtable_insert(ht, &h5->next, h5->hash);
    hash_table_check(ht, keyf);
    struct my_entry *h9 = he_alloc(9);
    p64_hashtable_insert(ht, &h9->next, h9->hash);
    hash_table_check(ht, keyf);

    p64_hazardptr_t hp;
    struct my_entry *me = (struct my_entry *)p64_hashtable_lookup(ht, compf, &(uint32_t){2}, hash(2), &hp);
    if (me != NULL)
    {
	printf("Found key %u node %p hazp %p (%p)\n", me->key, me, hp, *hp);
	hp_release_ro(&hp);
	assert(hp == P64_HAZARDPTR_NULL);
    }
    else
    {
	printf("Key %u not found\n", 2);
	assert(hp == P64_HAZARDPTR_NULL);
    }
    printf("hp_num_free()=%u\n", hp_num_free());

    me = (struct my_entry *)p64_hashtable_lookup(ht, compf, &(uint32_t){8}, hash(8), &hp);
    if (me != NULL)
    {
	printf("Found key %u node %p hazp %p (%p)\n", me->key, me, hp, *hp);
	hp_release_ro(&hp);
	assert(hp == P64_HAZARDPTR_NULL);
    }
    else
    {
	printf("Key %u not found\n", 8);
	assert(hp == P64_HAZARDPTR_NULL);
    }
    printf("hp_num_free()=%u\n", hp_num_free());

    me = (struct my_entry *)p64_hashtable_lookup(ht, compf, &(uint32_t){9}, hash(9), &hp);
    if (me != NULL)
    {
	printf("Found key %u node %p hazp %p (%p)\n", me->key, me, hp, *hp);
	hp_release_ro(&hp);
	assert(hp == P64_HAZARDPTR_NULL);
    }
    else
    {
	printf("Key %u not found\n", 9);
	assert(hp == P64_HAZARDPTR_NULL);
    }
    printf("hp_num_free()=%u\n", hp_num_free());

    printf("Remove 2\n");
    p64_hashtable_remove(ht, &h2->next, hash(2));
    hash_table_check(ht, keyf);
    printf("Remove 1\n");
    p64_hashtable_remove(ht, &h1->next, hash(1));
    hash_table_check(ht, keyf);
    printf("Remove 3\n");
    p64_hashtable_remove(ht, &h3->next, hash(3));
    hash_table_check(ht, keyf);
    printf("Remove 9\n");
    p64_hashtable_remove(ht, &h9->next, hash(9));
    hash_table_check(ht, keyf);
    printf("hp_num_free()=%u\n", hp_num_free());

    return 0;
}
