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

struct p64_ringbuf;
typedef struct p64_ringbuf p64_ringbuf_t;

//Allocate a ring buffer with space for at least 'nelems' elements
p64_ringbuf_t *p64_ringbuf_alloc(uint32_t nelems);

//Free a ring buffer
//The ring buffer must be empty
void p64_ringbuf_free(p64_ringbuf_t *rb);

//Enqueue elements on a ring buffer
//The number of actually enqueued elements is returned
int p64_ringbuf_enq(p64_ringbuf_t *rb, void *ev[], int num);

//Dequeue elements from a ring buffer
//The number of actually dequeued elements is returned
int p64_ringbuf_deq(p64_ringbuf_t *rb, void *ev[], int num);

#ifdef __cplusplus
}
#endif

#endif
