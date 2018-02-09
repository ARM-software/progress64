//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _P64_REORDER_H
#define _P64_REORDER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct p64_reorder p64_reorder_t;

typedef void (*p64_reorder_cb)(void *arg, void *elem);

//Allocate a reorder buffer with space for at least 'nelems' elements
p64_reorder_t *p64_reorder_alloc(uint32_t nelems,
				 p64_reorder_cb cb,
				 void *arg);

//Free a reorder buffer
//The reorder buffer must be empty
void p64_reorder_free(p64_reorder_t *rb);

//Reserver (consecutive) space in the reorder buffer
//Return amount of space actually reserved
//Write the first reserved slot number to '*sn'
uint32_t p64_reorder_reserve(p64_reorder_t *rb,
			     uint32_t nelems,
			     uint32_t *sn);

//Insert elements into the reorder buffer from the indicated position
//If possible retire in-order elements and invoke the callback
void p64_reorder_insert(p64_reorder_t *rb,
			uint32_t nelems,
			void *elems[],
			uint32_t sn);

#ifdef __cplusplus
}
#endif

#endif
