//Copyright (c) 2020, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

//Non-blocking cuckoo hash table with overflow cellar using linear probing

#ifndef _P64_CUCKOOHT_H
#define _P64_CUCKOOHT_H

#include <stdint.h>
#include <stdbool.h>
#include "p64_hazardptr.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define P64_CUCKOOHT_F_HP      0x0001 //Use hazard pointers (default QSBR)

typedef uintptr_t p64_cuckoohash_t;

//Each element in the hash table must include a p64_cuckooelem field
typedef struct p64_cuckooelem
{
    p64_cuckoohash_t hash;
} p64_cuckooelem_t;

typedef struct p64_cuckooht p64_cuckooht_t;

typedef int (*p64_cuckooht_compare)(const p64_cuckooelem_t *elem,
				    const void *key);

typedef void (*p64_cuckooht_trav_cb)(void *arg,
				     p64_cuckooelem_t *elem,
				     size_t idx);

//Allocate a hash table with space for at least 'nelems' elements in the main
//hash table and 'ncells' elements in the backup cellar
//Specify a key compare function which is used when hashes are identical
p64_cuckooht_t *
p64_cuckooht_alloc(size_t nelems,
		   size_t ncells,
		   p64_cuckooht_compare cf,
		   uint32_t flags);

//Free a hash table
//The hash table must be empty
void
p64_cuckooht_free(p64_cuckooht_t *);

//Lookup an element in the hash table, given the key and a hash value of the key
//If the element is found, the hazard pointer will contain a reference which
//must eventually be released
//Caller must call QSBR acquire/release/quiescent as appropriate
p64_cuckooelem_t *
p64_cuckooht_lookup(p64_cuckooht_t *ht,
		    const void *key,
		    p64_cuckoohash_t hash,
		    p64_hazardptr_t *hp);

#if 0
//Look up a vector of elements, given the keys and corresponding hashes
//Return bitmask with successful lookups (result[i] != NULL)
//Must only be used with QSBR!
//Caller must call QSBR acquire/release/quiescent as appropriate
unsigned long
p64_cuckooht_lookup_vec(p64_cuckooht_t *ht,
			uint32_t num,
			const void *keys[num],
			p64_cuckoohash_t hashes[num],
			p64_cuckooelem_t *result[num]);
#endif

//Insert an element into the hash table
//Return true if insertion succeeds, false otherwise
//Element must be 32-byte aligned (5-lsb must be zero)
bool
p64_cuckooht_insert(p64_cuckooht_t *ht,
		    p64_cuckooelem_t *elem,
		    p64_cuckoohash_t hash);

//Remove specified element
//Return true if removal successful, false otherwise (element not found)
bool
p64_cuckooht_remove(p64_cuckooht_t *ht,
		    p64_cuckooelem_t *elem,
		    p64_cuckoohash_t hash);

#if 0
//Remove and return element specified by key & hash
//Return NULL if element not found
//Caller must call QSBR acquire/release/quiescent as appropriate
void *
p64_cuckooht_remove_by_key(p64_cuckooht_t *ht,
			   const void *key,
			   p64_cuckoohash_t hash,
			   p64_hazardptr_t *hp);
#endif

//Traverse hash table, calling user-defined call-back for every element
void
p64_cuckooht_traverse(p64_cuckooht_t *ht,
		       p64_cuckooht_trav_cb cb,
		       void *arg);

#ifdef __cplusplus
}
#endif

#endif
