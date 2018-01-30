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

//Each object in the hash table must include a p64_hashentry field
struct p64_hashentry
{
    p64_hashvalue_t hash;//Hash value for entry pointed by next pointer
    struct p64_hashentry *next;
};

struct p64_hashtable;

typedef int (*p64_hashtable_compare)(const struct p64_hashentry *,
				     const void *key);

struct p64_hashtable *p64_hashtable_alloc(uint32_t nentries);

bool p64_hashtable_free(struct p64_hashtable *);

struct p64_hashentry *p64_hashtable_lookup(struct p64_hashtable *ht, 
					   p64_hashtable_compare cf,
					   const void *key,
					   p64_hashvalue_t hash,
					   p64_hazardptr_t *hp);

#ifndef NDEBUG
#define p64_hashtable_lookup(_a, _b, _c, _d, _e) \
({ \
     p64_hazardptr_t *_f = (_e); \
     struct p64_hashentry *_g = p64_hashtable_lookup((_a), (_b), (_c), (_d), _f); \
     if (*(_f) != P64_HAZARDPTR_NULL) \
	 p64_hazptr_annotate(*(_f), __FILE__, __LINE__); \
     _g; \
})
#endif

void p64_hashtable_insert(struct p64_hashtable *ht,
			  struct p64_hashentry *he,
			  p64_hashvalue_t hash);

//Remove specified hashentry
//Return false if removal fails, entry not found
bool p64_hashtable_remove(struct p64_hashtable *ht,
			  struct p64_hashentry *he,
			  p64_hashvalue_t hash);

//Remove and return hashentry specified by key
//Return NULL if entry not found
struct p64_hashentry *p64_hashtable_remove_by_key(struct p64_hashtable *ht,
						  p64_hashtable_compare cf,
						  const void *key,
						  p64_hashvalue_t hash,
						  p64_hazardptr_t *hp);

#ifndef NDEBUG
#define p64_hashtable_remove_by_key(_a, _b, _c, _d, _e) \
({ \
     p64_hazardptr_t *_f = (_e); \
     struct p64_hashentry *_g = p64_hashtable_remove_by_key((_a), (_b), (_c), (_d), _f); \
     if (*(_f) != P64_HAZARDPTR_NULL) \
	 p64_hazptr_annotate(*(_f), __FILE__, __LINE__); \
     _g; \
})
#endif

#ifdef __cplusplus
}
#endif

#endif
