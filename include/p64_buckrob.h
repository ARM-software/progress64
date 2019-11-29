//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

//A scalable non-blocking reorder buffer using pass-the-buck algorithm

#ifndef _P64_BUCKROB_H
#define _P64_BUCKROB_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

//Reserved element pointer, don't use!
#define P64_BUCKROB_RESERVED_ELEM ((void *)1U)

typedef struct p64_buckrob p64_buckrob_t;

//Callback for in-order elements
//Called with NULL elem to conclude a sequence of calls with non-NULL elem
typedef void (*p64_buckrob_cb)(void *arg, void *elem, uint32_t sn);

//Allocate a buckrob buffer with space for at least 'nelems' elements
p64_buckrob_t *p64_buckrob_alloc(uint32_t nelems,
				 bool user_acquire,
				 p64_buckrob_cb cb,
				 void *arg);

//Free a reorder buffer
//The reorder buffer must be empty
void p64_buckrob_free(p64_buckrob_t *rob);

//Acquire space in the reorder buffer
//Return amount of space actually acquired
//Write the first acquired slot number to '*sn'
uint32_t p64_buckrob_acquire(p64_buckrob_t *rob,
			     uint32_t nelems,
			     uint32_t *sn);

//Insert elements into the reorder buffer from the indicated position
//If possible release in-order elements and invoke the callback
void p64_buckrob_release(p64_buckrob_t *rob,
			 uint32_t sn,
			 void *elems[],
			 uint32_t nelems);

#ifdef __cplusplus
}
#endif

#endif
