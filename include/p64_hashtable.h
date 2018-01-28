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

#ifdef __cplusplus
}
#endif

#endif
