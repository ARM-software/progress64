//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _P64_RINGBUF_H
#define _P64_RINGBUF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef void *p64_element;
struct p64_ringbuf;

struct p64_ringbuf *p64_ringbuf_alloc(uint32_t ring_size);
bool p64_ringbuf_free(struct p64_ringbuf *rb);
int p64_ringbuf_enq(struct p64_ringbuf *rb, const p64_element ev[], int num);
int p64_ringbuf_deq(struct p64_ringbuf *rb, p64_element ev[], int num);

#ifdef __cplusplus
}
#endif

#endif
