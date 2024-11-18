//Copyright (c) 2024, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

//Blocking MP/MC ring buffer
//Enqueue will block until all requested elements have been enqueued into empty slots
//Dequeue will block until all requested elements have been dequeued from filled slots
//Enqueue only accesses one shared memory location for best scalability (ditto for dequeue)

#ifndef P64_BLKRING_H
#define P64_BLKRING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct p64_blkring p64_blkring_t;

//Allocate ring buffer with space for at least 'nelems' elements
//'nelems' != 0 and 'nelems' <= 0x80000000
p64_blkring_t *
p64_blkring_alloc(uint32_t nelems);

//Free ring buffer
//The ring buffer must be empty
void
p64_blkring_free(p64_blkring_t *rb);

//Enqueue elements on ring buffer
//The function will block until all elements have been enqueued
void
p64_blkring_enqueue(p64_blkring_t *rb, void *const ev[], uint32_t num);

//Dequeue elements from ring buffer
//The function will block until all elements have been dequeued
void
p64_blkring_dequeue(p64_blkring_t *rb, void *ev[], uint32_t num, uint32_t *index);

//Dequeue elements from ring buffer
//Truncate 'num' against number of available elements
//This function has less scalability as both ring buffer 'head' and 'tail' must be accessed
uint32_t
p64_blkring_dequeue_nblk(p64_blkring_t *rb, void *ev[], uint32_t num, uint32_t *index);

#ifdef __cplusplus
}
#endif

#endif
