//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _P64_RINGBUF_H
#define _P64_RINGBUF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define P64_RINGBUF_F_MPENQ      0x0000 //Multi producer
#define P64_RINGBUF_F_SPENQ      0x0001 //Single producer
#define P64_RINGBUF_F_MCDEQ      0x0000 //Multi consumer
#define P64_RINGBUF_F_SCDEQ      0x0002 //Single consumer
#define P64_RINGBUF_F_LFDEQ      0x0004 //Lock-free multi consumer

typedef struct p64_ringbuf p64_ringbuf_t;

typedef struct p64_ringbuf_result
{
    uint32_t actual;
    uint32_t index;
    uint32_t mask;
} p64_ringbuf_result_t;

//Allocate a ring buffer with space for at least 'nelems' elements
//of size 'esize' each
//'nelems' != 0 and 'nelems' <= 0x80000000
p64_ringbuf_t *
p64_ringbuf_alloc(uint32_t nelems, uint32_t flags, size_t esize);

//Free a ring buffer
//The ring buffer must be empty
void
p64_ringbuf_free(p64_ringbuf_t *rb);

//Enqueue elements on a ring buffer
//The number of actually enqueued elements is returned
uint32_t
p64_ringbuf_enqueue(p64_ringbuf_t *rb, const void *ev[], uint32_t num);

//Dequeue elements from a ring buffer
//The number of actually dequeued elements is returned
uint32_t
p64_ringbuf_dequeue(p64_ringbuf_t *rb, void *ev[], uint32_t num,
		    uint32_t *index);

//Special functions used by templates
void *
p64_ringbuf_alloc_(uint32_t nelems, uint32_t flags, size_t esize);

void
p64_ringbuf_free_(void *ptr);

p64_ringbuf_result_t
p64_ringbuf_acquire_(void *ptr, uint32_t num, bool enqueue);

bool
p64_ringbuf_release_(void *ptr, p64_ringbuf_result_t, bool enqueue);

#ifdef __cplusplus
}
#endif

#endif
