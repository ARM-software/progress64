//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _P64_RINGBUF_TEMPLATE_H
#define _P64_RINGBUF_TEMPLATE_H

#include "p64_ringbuf.h"

#undef _CONCAT
#define _CONCAT(x, y) x ## y

#define P64_RINGBUF(_name, _type) \
typedef void *_CONCAT(_name,_t); \
\
static inline _CONCAT(_name,_t) \
_CONCAT(_name,_alloc)(uint32_t nelems, uint32_t flags) \
{ \
    return (_CONCAT(_name,_t))p64_ringbuf_alloc_(nelems, flags, sizeof(_type)); \
} \
\
static inline void \
_CONCAT(_name,_free)(_CONCAT(_name,_t) rb) \
{ \
    p64_ringbuf_free_((rb)); \
} \
\
static inline uint32_t \
_CONCAT(_name,_enqueue)(_CONCAT(_name,_t) rb, const _type ev[], uint32_t num) \
{ \
    const struct p64_ringbuf_result r = p64_ringbuf_acquire_(rb, num, true); \
    if (r.actual != 0) \
    { \
	_type *ring = (_type *)rb; \
	for (uint32_t i = 0; i < r.actual; i++) \
	{ \
	    ring[(r.index + i) & r.mask] = ev[i]; \
	} \
	(void)p64_ringbuf_release_(rb, r, true); \
    } \
    return r.actual; \
} \
\
static inline uint32_t \
_CONCAT(_name,_dequeue)(_CONCAT(_name,_t) rb, _type ev[], uint32_t num, uint32_t *index) \
{ \
    struct p64_ringbuf_result r; \
    do \
    { \
	r = p64_ringbuf_acquire_(rb, num, false); \
	if (r.actual == 0) \
	{ \
	    return r.actual; \
	} \
	const _type *ring = (const _type *)rb; \
	*index = r.index; \
	for (uint32_t i = 0; i < r.actual; i++) \
	{ \
	    ev[i] = ring[(r.index + i) & r.mask]; \
	} \
    } \
    while (!p64_ringbuf_release_(rb, r, false)); \
    return r.actual; \
}

#endif
