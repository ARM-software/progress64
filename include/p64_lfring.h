//Copyright (c) 2018-2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

//Lock-free MP/MC ring buffer
//SP/SC modes are also supported

#ifndef _P64_LFRING_H
#define _P64_LFRING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define P64_LFRING_F_MPENQ      0x0000 //Multi producer
#define P64_LFRING_F_SPENQ      0x0001 //Single producer
#define P64_LFRING_F_MCDEQ      0x0000 //Multi consumer
#define P64_LFRING_F_SCDEQ      0x0002 //Single consumer

typedef struct p64_lfring p64_lfring_t;

//Allocate ring buffer with space for at least 'nelems' elements
//'nelems' != 0 and 'nelems' <= 0x80000000
p64_lfring_t *
p64_lfring_alloc(uint32_t nelems, uint32_t flags);

//Free ring buffer
//The ring buffer must be empty
void
p64_lfring_free(p64_lfring_t *lfr);

//Enqueue elements on ring buffer
//The number of actually enqueued elements is returned
uint32_t
p64_lfring_enqueue(p64_lfring_t *lfr,
		   void *const elems[],
		   uint32_t nelems);

//Dequeue elements from ring buffer
//The number of actually dequeued elements is returned
uint32_t
p64_lfring_dequeue(p64_lfring_t *lfr,
		   void *elems[],
		   uint32_t nelems,
		   uint32_t *index);

#ifdef __cplusplus
}
#endif

#endif
