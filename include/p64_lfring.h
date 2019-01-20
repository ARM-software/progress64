//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _P64_LFRING_H
#define _P64_LFRING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct p64_lfring p64_lfring_t;

//Allocate a ring buffer with space for at least 'nelems' elements
//'nelems' != 0 and 'nelems' <= 0x80000000
p64_lfring_t *
p64_lfring_alloc(uint32_t nelems);

//Free a ring buffer
//The ring buffer must be empty
void
p64_lfring_free(p64_lfring_t *lfr);

//Enqueue elements on a ring buffer
//The number of actually enqueued elements is returned
uint32_t
p64_lfring_enqueue(p64_lfring_t *lfr,
		   void *const elems[],
		   uint32_t nelems);

//Dequeue elements from a ring buffer
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
