//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

//Non-blocking 'strict' reorder buffer

#ifndef _P64_REORDER_H
#define _P64_REORDER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define P64_REORDER_DUMMY ((void *)1U)

typedef struct p64_reorder p64_reorder_t;

//Callback for in-order elements
//Called with NULL elem to conclude a sequence of calls with non-NULL elem
typedef void (*p64_reorder_cb)(void *arg, void *elem, uint32_t sn);

//Allocate a reorder buffer with space for at least 'nelems' elements
p64_reorder_t *p64_reorder_alloc(uint32_t nelems,
				 bool user_acquire,
				 p64_reorder_cb cb,
				 void *arg);

//Free a reorder buffer
//The reorder buffer must be empty
void p64_reorder_free(p64_reorder_t *rob);

//Acquire (consecutive) space in the reorder buffer
//Return amount of space actually acquired
//Write the first acquired slot number to '*sn'
uint32_t p64_reorder_acquire(p64_reorder_t *rob,
			     uint32_t nelems,
			     uint32_t *sn);

//Insert elements into the reorder buffer from the indicated position
//If possible release in-order elements and invoke the callback
void p64_reorder_release(p64_reorder_t *rob,
			 uint32_t sn,
			 void *elems[],
			 uint32_t nelems);

#ifdef __cplusplus
}
#endif

#endif
