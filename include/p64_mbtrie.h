//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

//Longest prefix match (LPM) using non-blocking multi-bit trie
//Max prefix length is 64 bits
//Most significant bits of 64-bit key are used for lookup
//Elements must be 64B-aligned as 6 lsb are used internally
//Elements represent next-hop information

#ifndef _P64_MBTRIE_H
#define _P64_MBTRIE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "p64_hazardptr.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define P64_MBTRIE_F_HP      0x0001 //Use hazard pointers (default QSBR)

typedef struct p64_mbtrie_elem
{
    size_t refcnt;
} p64_mbtrie_elem_t;

typedef struct p64_mbtrie p64_mbtrie_t;

typedef void (*p64_mbtrie_free_cb)(void *arg, p64_mbtrie_elem_t *elem);

typedef void (*p64_mbtrie_trav_cb)(void *arg, uint64_t pfx, uint32_t pfxlen, p64_mbtrie_elem_t *elem, uint32_t actlen);

p64_mbtrie_t *
p64_mbtrie_alloc(const uint8_t *strides,//0-terminated array of stride sizes
		 p64_mbtrie_free_cb refcnt_zero_cb,
		 void *refcnt_zero_arg,
		 uint32_t flags);

void
p64_mbtrie_free(p64_mbtrie_t *mbt);

//Insert element per its prefix, concealing elements with shorter or equal
//prefix length
void
p64_mbtrie_insert(p64_mbtrie_t *mbt,
		  uint64_t pfx,
		  uint32_t pfxlen,
		  p64_mbtrie_elem_t *elem);

//Remove 'old' element, replacing it with 'new' element (may be NULL)
void
p64_mbtrie_remove(p64_mbtrie_t *mbt,
		  uint64_t pfx,
		  uint32_t pfxlen,
		  p64_mbtrie_elem_t *old,
		  p64_mbtrie_elem_t *new);

//Look up the specified key, returning the matching element (or NULL)
//The hazard pointer must be freed even if NULL is returned
//Key starts from most significant bit in uint64_t value
//Must only be used with hazard pointers!
p64_mbtrie_elem_t *
p64_mbtrie_lookup(p64_mbtrie_t *mbt,
		  uint64_t key,
		  p64_hazardptr_t *hp);
#ifndef NDEBUG
#define p64_mbtrie_lookup(_a, _b, _c) \
({ \
     p64_hazardptr_t *_d = (_c); \
     p64_mbtrie_elem_t *_e = p64_mbtrie_lookup((_a), (_b), _d); \
     if (*(_d) != P64_HAZARDPTR_NULL) \
         p64_hazptr_annotate(*(_d), __FILE__, __LINE__); \
     _e; \
})
#endif

//Look up a vector of keys (up to 32/64 elements), returning matching elements
//(or NULL)
//Key starts from most significant bit in uint64_t values
//Return bitmask with successful lookups (result[i] != NULL)
//Must only be used with QSBR!
unsigned long
p64_mbtrie_lookup_vec(p64_mbtrie_t *mbt,
                      uint32_t num,
                      uint64_t keys[num],
                      p64_mbtrie_elem_t *result[num]);

//Traverse mbtrie, calling user-defined call-back for every valid prefix
//real_refs=true => only call-back for actual references to mbtrie elements
void
p64_mbtrie_traverse(p64_mbtrie_t *mbt,
		    p64_mbtrie_trav_cb cb,
		    void *arg,
		    bool real_refs);

#ifdef __cplusplus
}
#endif

#endif
