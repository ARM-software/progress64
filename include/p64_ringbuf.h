//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

//Classic blocking MP/MC ring buffer
//Non-blocking (but not lock-free) MP/MC mode also supported
//SP/SC modes are also supported
//The element size must be sizeof(void *)

#ifndef _P64_RINGBUF_H
#define _P64_RINGBUF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

//In non-blocking mode, the following element value is invalid
#define P64_RINGBUF_INVALID_ELEM NULL

#define P64_RINGBUF_F_MPENQ      0x0000 //Blocking multi producer enqueue
#define P64_RINGBUF_F_MCDEQ      0x0000 //Blocking multi consumer dequeue
#define P64_RINGBUF_F_SPENQ      0x0001 //Single producer enqueue (non MT-safe)
#define P64_RINGBUF_F_SCDEQ      0x0002 //Single consumer dequeue (non MT-safe)
#define P64_RINGBUF_F_LFDEQ      0x0004 //Lock-free multi consumer dequeue
#define P64_RINGBUF_F_NBENQ      0x0008 //Non-blocking multi-producer enqueue
#define P64_RINGBUF_F_NBDEQ      0x0010 //Non-blocking multi-consumer dequeue

typedef struct p64_ringbuf p64_ringbuf_t;

typedef struct p64_ringbuf_result
{
    uint32_t actual;
    uint32_t index;
    uint32_t mask;
} p64_ringbuf_result_t;

//Allocate ring buffer with space for at least 'nelems' elements
//of size 'esize' each
//'nelems' != 0 and 'nelems' <= 0x80000000
p64_ringbuf_t *
p64_ringbuf_alloc(uint32_t nelems, uint32_t flags, size_t esize);

//Free ring buffer
//The ring buffer must be empty
void
p64_ringbuf_free(p64_ringbuf_t *rb);

//Enqueue elements on ring buffer
//Return the number of actually enqueued elements
uint32_t
p64_ringbuf_enqueue(p64_ringbuf_t *rb, void *const ev[], uint32_t num);

//Dequeue elements from ring buffer
//Return the number of actually dequeued elements
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
