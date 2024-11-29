//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

//Blocking ring buffer with user defined element type
//Supports blocking MP/MC and SP/SC modes, also lock-free MC dequeue

#ifndef P64_RINGBUF_TEMPLATE_H
#define P64_RINGBUF_TEMPLATE_H

#include "p64_ringbuf.h"

#ifndef P64_CONCAT
#define P64_CONCAT(x, y) x ## y
#endif

#undef UNROLL_LOOPS
#ifdef __clang__
#define UNROLL_LOOPS __attribute__((opencl_unroll_hint(4)))
#elif defined __GNUC__
#define UNROLL_LOOPS __attribute__((optimize("unroll-loops")))
#else
#define UNROLL_LOOPS
#endif

#define P64_RINGBUF(_name, _type) \
typedef p64_ringbuf_t P64_CONCAT(_name,_t); \
\
static inline P64_CONCAT(_name,_t) * \
P64_CONCAT(_name,_alloc)(uint32_t nelems, uint32_t flags) \
{ \
    return (P64_CONCAT(_name,_t) *)p64_ringbuf_alloc_(nelems, flags, sizeof(_type)); \
} \
\
static inline void \
P64_CONCAT(_name,_free)(P64_CONCAT(_name,_t) *rb) \
{ \
    p64_ringbuf_free_((rb)); \
} \
\
UNROLL_LOOPS \
static inline void \
P64_CONCAT(_name,_copy)(_type *restrict dst, const _type *restrict src, uint32_t num) \
{ \
    for (uint32_t i = 0; i < num; i++) \
    { \
	dst[i] = src[i]; \
    } \
} \
\
static inline uint32_t \
P64_CONCAT(_name,_enqueue)(P64_CONCAT(_name,_t) *rb, _type const ev[], uint32_t num) \
{ \
    const struct p64_ringbuf_result r = p64_ringbuf_acquire_(rb, num, true); \
    if (r.actual != 0) \
    { \
	uint32_t seg0 = r.mask + 1 - (r.index & r.mask); \
	_type *ring = (_type *)r.ring; \
	_type *ring0 = &ring[r.index & r.mask]; \
	if (r.actual <= seg0) \
	{ \
	    /* One contiguous range */ \
	    P64_CONCAT(_name,_copy)(ring0, ev, r.actual); \
	} \
	else \
	{ \
	    /* Range wraps around end of ring => two subranges */ \
	    P64_CONCAT(_name,_copy)(ring0, ev, seg0); \
	    P64_CONCAT(_name,_copy)(ring, ev + seg0, r.actual - seg0); \
	} \
	(void)p64_ringbuf_release_(rb, r, true); \
    } \
    return r.actual; \
} \
\
static inline uint32_t \
P64_CONCAT(_name,_dequeue)(P64_CONCAT(_name,_t) *rb, _type ev[], uint32_t num, uint32_t *index) \
{ \
    struct p64_ringbuf_result r; \
    do \
    { \
	r = p64_ringbuf_acquire_(rb, num, false); \
	if (r.actual == 0) \
	{ \
	    return r.actual; \
	} \
	*index = r.index; \
	uint32_t seg0 = r.mask + 1 - (r.index & r.mask); \
	_type *ring = (_type *)r.ring; \
	const _type *ring0 = &ring[r.index & r.mask]; \
	if (r.actual <= seg0) \
	{ \
	    /* One contiguous range */ \
	    P64_CONCAT(_name,_copy)(ev, ring0, r.actual); \
	} \
	else \
	{ \
	    /* Range wraps around end of ring => two subranges */ \
	    P64_CONCAT(_name,_copy)(ev, ring0, seg0); \
	    P64_CONCAT(_name,_copy)(ev + seg0, ring, r.actual - seg0); \
	} \
    } \
    while (!p64_ringbuf_release_(rb, r, false)); \
    return r.actual; \
}

#endif
