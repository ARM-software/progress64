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

#define P64_RINGBUF_FLAG_MP      0x0000 //Multi producer
#define P64_RINGBUF_FLAG_SP      0x0001 //Single producer
#define P64_RINGBUF_FLAG_MC      0x0000 //Multi consumer
#define P64_RINGBUF_FLAG_SC      0x0002 //Single consumer
#define P64_RINGBUF_FLAG_LFC     0x0004 //Lock-free consume

typedef struct p64_ringbuf p64_ringbuf_t;

//Allocate a ring buffer with space for at least 'nelems' elements
//'nelems' != 0 and 'nelems' <= 0x80000000
p64_ringbuf_t *p64_ringbuf_alloc(uint32_t nelems, uint32_t flags);

//Free a ring buffer
//The ring buffer must be empty
void p64_ringbuf_free(p64_ringbuf_t *rb);

//Enqueue elements on a ring buffer
//The number of actually enqueued elements is returned
uint32_t p64_ringbuf_enqueue(p64_ringbuf_t *rb, void *ev[], uint32_t num);

//Dequeue elements from a ring buffer
//The number of actually dequeued elements is returned
uint32_t p64_ringbuf_dequeue(p64_ringbuf_t *rb, void *ev[], uint32_t num);

#ifdef __cplusplus
}
#endif

#endif
