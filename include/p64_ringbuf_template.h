//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _P64_RINGBUF_TEMPLATE_H
#define _P64_RINGBUF_TEMPLATE_H

#include "p64_ringbuf.h"

#ifndef P64_CONCAT
#define P64_CONCAT(x, y) x ## y
#endif

#define P64_RINGBUF(_name, _type) \
typedef struct _name \
{ \
    _type ring[1]; /* Can't use flexible array member */ \
} P64_CONCAT(_name,_t); \
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
static inline uint32_t \
P64_CONCAT(_name,_enqueue)(P64_CONCAT(_name,_t) *rb, const _type ev[], uint32_t num) \
{ \
    const struct p64_ringbuf_result r = p64_ringbuf_acquire_(rb, num, true); \
    if (r.actual != 0) \
    { \
	for (uint32_t i = 0; i < r.actual; i++) \
	{ \
	    rb->ring[(r.index + i) & r.mask] = ev[i]; \
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
	for (uint32_t i = 0; i < r.actual; i++) \
	{ \
	    ev[i] = rb->ring[(r.index + i) & r.mask]; \
	} \
    } \
    while (!p64_ringbuf_release_(rb, r, false)); \
    return r.actual; \
}

#endif
