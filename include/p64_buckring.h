//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause
//
//Non-blocking ring buffer using pass-the-buck algorithm

#ifndef _P64_BUCKRING_H
#define _P64_BUCKRING_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct p64_buckring p64_buckring_t;

//Allocate a buck ring buffer with space for at least 'nelems' elements
//'nelems' != 0 and 'nelems' <= 0x80000000
p64_buckring_t *
p64_buckring_alloc(uint32_t nelems, uint32_t flags);

//Free a buck ring buffer
//The ring buffer must be empty
void
p64_buckring_free(p64_buckring_t *rb);

//Enqueue elements on a buck ring buffer
//Return the number of actually enqueued elements
uint32_t
p64_buckring_enqueue(p64_buckring_t *rb, void *const ev[], uint32_t num);

//Dequeue elements from a buck ring buffer
//Return the number of actually dequeued elements
uint32_t
p64_buckring_dequeue(p64_buckring_t *rb, void *ev[], uint32_t num,
		     uint32_t *index);

#ifdef __cplusplus
}
#endif

#endif
