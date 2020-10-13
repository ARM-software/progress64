//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

//Non-blocking hopscotch hash table with overflow cellar using linear probing
//Original hopscotch design from Herlihy, Shavit, Tzafrir: "Hopscotch Hashing"

#ifndef P64_HOPSCOTCH_H
#define P64_HOPSCOTCH_H

#include <stdint.h>
#include <stdbool.h>
#include "p64_hazardptr.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define P64_HOPSCOTCH_F_HP      0x0001 //Use hazard pointers (default QSBR)

typedef uintptr_t p64_hopschash_t;

typedef struct p64_hopscotch p64_hopscotch_t;

typedef int (*p64_hopscotch_compare)(const void *elem,
				     const void *key);

typedef void (*p64_hopscotch_trav_cb)(void *arg,
				      void *elem,
				      size_t idx);

//Allocate a hash table with space for at least 'nbkts' elements in the main
//hash table and 'ncells' elements in the backup cellar
//Specify a key compare function which is used when hashes are identical
p64_hopscotch_t *
p64_hopscotch_alloc(size_t nbkts,
		    size_t ncells,
		    p64_hopscotch_compare cf,
		    uint32_t flags);

//Free a hash table
//The hash table must be empty
void
p64_hopscotch_free(p64_hopscotch_t *);

//Lookup an element in the hash table, given the key and a hash value of the key
//If using hazard pointers, the hazard pointer must always be released, even
//when no matching element was found
//If using QSBR, the caller must call QSBR acquire/release/quiescent as
//appropriate
void *
p64_hopscotch_lookup(p64_hopscotch_t *ht,
		     const void *key,
		     p64_hopschash_t hash,
		     p64_hazardptr_t *hp);

#ifndef NDEBUG
#define p64_hopscotch_lookup(_ht, _k, _ha, _d) \
({ \
     p64_hazardptr_t *_hp = (_d); \
     void *_el = p64_hopscotch_lookup((_ht), (_k), (_ha), _hp); \
     if (*(_hp) != P64_HAZARDPTR_NULL) \
	 p64_hazptr_annotate(*(_hp), __FILE__, __LINE__); \
     _el; \
})
#endif

//Look up a vector of elements, given the keys and corresponding hashes
//Return bitmask with successful lookups (result[i] != NULL)
//Must only be used with QSBR!
//Caller must call QSBR acquire/release/quiescent as appropriate
void
p64_hopscotch_lookup_vec(p64_hopscotch_t *ht,
			 uint32_t num,
			 const void *keys[num],
			 p64_hopschash_t hashes[num],
			 void *result[num]);

//Insert an element into the hash table
//Return true if insertion succeeds, false otherwise
bool
p64_hopscotch_insert(p64_hopscotch_t *ht,
		     void *elem,
		     p64_hopschash_t hash);

//Remove specified element
//Return true if removal successful, false otherwise (element not found)
bool
p64_hopscotch_remove(p64_hopscotch_t *ht,
		     void *elem,
		     p64_hopschash_t hash);

//Remove and return element specified by key & hash
//Return NULL if element not found
//Caller must call QSBR acquire/release/quiescent as appropriate
void *
p64_hopscotch_remove_by_key(p64_hopscotch_t *ht,
			    const void *key,
			    p64_hopschash_t hash,
			    p64_hazardptr_t *hp);

#ifndef NDEBUG
#define p64_hopscotch_remove_by_key(_ht, _k, _ha, _e) \
({ \
     p64_hazardptr_t *_hp = (_e); \
     void *_el = p64_hopscotch_remove_by_key((_ht), (_k), (_ha), _hp); \
     if (*(_hp) != P64_HAZARDPTR_NULL) \
	 p64_hazptr_annotate(*(_hp), __FILE__, __LINE__); \
     _el; \
})
#endif

//Traverse hash table, calling user-defined call-back for every element
void
p64_hopscotch_traverse(p64_hopscotch_t *ht,
		       p64_hopscotch_trav_cb cb,
		       void *arg);

#ifdef __cplusplus
}
#endif

#endif
