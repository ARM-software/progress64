//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _P64_HASHTABLE_H
#define _P64_HASHTABLE_H

#include <stdint.h>
#include <stdbool.h>
#include "p64_hazardptr.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define P64_HASHTAB_F_HP      0x0001 //Use hazard pointers (default QSBR)

typedef uintptr_t p64_hashvalue_t;

//Each element in the hash table must include a p64_hashelem field
typedef struct p64_hashelem
{
    _Alignas(2 * sizeof(void *))
    p64_hashvalue_t hash;//Hash value for element pointed by next pointer
    struct p64_hashelem *next;
} p64_hashelem_t;

typedef struct p64_hashtable p64_hashtable_t;

typedef int (*p64_hashtable_compare)(const p64_hashelem_t *he,
				     const void *key);

typedef void (*p64_hashtable_trav_cb)(void *arg,
				      p64_hashelem_t *he,
				      size_t idx);

//Allocate a hash table with space for at least 'nelems' elements
//Specify a key compare function which is used when hashes are identical
p64_hashtable_t *p64_hashtable_alloc(size_t nelems,
				     p64_hashtable_compare cf,
				     uint32_t flags);

//Free a hash table
//The hash table must be empty
void p64_hashtable_free(p64_hashtable_t *);

//Lookup an element in the hash table, given the key and a hash value of the key
//If the element is found, the hazard pointer will contain a reference which
//must eventually be released
//Caller must call QSBR acquire/release/quiescent as appropriate
p64_hashelem_t *p64_hashtable_lookup(p64_hashtable_t *ht,
				     const void *key,
				     p64_hashvalue_t hash,
				     p64_hazardptr_t *hp);

#ifndef NDEBUG
#define p64_hashtable_lookup(_ht, _k, _ha, _d) \
({ \
     p64_hazardptr_t *_hp = (_d); \
     p64_hashelem_t *_el = p64_hashtable_lookup((_ht), (_k), (_ha), _hp); \
     if (*(_hp) != P64_HAZARDPTR_NULL) \
	 p64_hazptr_annotate(*(_hp), __FILE__, __LINE__); \
     _el; \
})
#endif

//Insert an element into the hash table
void p64_hashtable_insert(p64_hashtable_t *ht,
			  p64_hashelem_t *he,
			  p64_hashvalue_t hash);

//Remove specified element
//Return false if removal fails, element not found
bool p64_hashtable_remove(p64_hashtable_t *ht,
			  p64_hashelem_t *he,
			  p64_hashvalue_t hash);

//Remove and return element specified by key
//Return NULL if element not found
//Caller must call QSBR acquire/release/quiescent as appropriate
p64_hashelem_t *p64_hashtable_remove_by_key(p64_hashtable_t *ht,
					    const void *key,
					    p64_hashvalue_t hash,
					    p64_hazardptr_t *hp);

#ifndef NDEBUG
#define p64_hashtable_remove_by_key(_ht, _k, _ha, _e) \
({ \
     p64_hazardptr_t *_hp = (_e); \
     p64_hashelem_t *_el = p64_hashtable_remove_by_key((_ht), (_k), (_ha), _hp); \
     if (*(_hp) != P64_HAZARDPTR_NULL) \
	 p64_hazptr_annotate(*(_hp), __FILE__, __LINE__); \
     _el; \
})
#endif

//Traverse hash table, calling user-defined call-back for every element
void
p64_hashtable_traverse(p64_hashtable_t *ht,
		       p64_hashtable_trav_cb cb,
		       void *arg);

#ifdef __cplusplus
}
#endif

#endif
