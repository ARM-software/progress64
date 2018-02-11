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

#if __SIZEOF_POINTER__ == 8
typedef uint64_t p64_hashvalue_t;
#else
#error Unsupported pointer size
#endif

//Each element in the hash table must include a p64_hashelem field
typedef struct p64_hashelem
{
    p64_hashvalue_t hash;//Hash value for element pointed by next pointer
    struct p64_hashelem *next;
} p64_hashelem_t;

typedef struct p64_hashtable p64_hashtable_t;

typedef int (*p64_hashtable_compare)(const p64_hashelem_t *,
				     const void *key);

//Allocate a hash table with space for at least 'nelems' elements
p64_hashtable_t *p64_hashtable_alloc(uint32_t nelems);

//Free a hash table
//The hash table must be empty
void p64_hashtable_free(p64_hashtable_t *);

p64_hashelem_t *p64_hashtable_lookup(p64_hashtable_t *ht,
				     p64_hashtable_compare cf,
				     const void *key,
				     p64_hashvalue_t hash,
				     p64_hazardptr_t *hp);

#ifndef NDEBUG
#define p64_hashtable_lookup(_a, _b, _c, _d, _e) \
({ \
     p64_hazardptr_t *_f = (_e); \
     p64_hashelem_t *_g = p64_hashtable_lookup((_a), (_b), (_c), (_d), _f); \
     if (*(_f) != P64_HAZARDPTR_NULL) \
	 p64_hazptr_annotate(*(_f), __FILE__, __LINE__); \
     _g; \
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
p64_hashelem_t *p64_hashtable_remove_by_key(p64_hashtable_t *ht,
						 p64_hashtable_compare cf,
						 const void *key,
						 p64_hashvalue_t hash,
						 p64_hazardptr_t *hp);

#ifndef NDEBUG
#define p64_hashtable_remove_by_key(_a, _b, _c, _d, _e) \
({ \
     p64_hazardptr_t *_f = (_e); \
     p64_hashelem_t *_g = p64_hashtable_remove_by_key((_a), (_b), (_c), (_d), _f); \
     if (*(_f) != P64_HAZARDPTR_NULL) \
	 p64_hazptr_annotate(*(_f), __FILE__, __LINE__); \
     _g; \
})
#endif

#ifdef __cplusplus
}
#endif

#endif
