//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _P64_LAXROB_H
#define _P64_LAXROB_H

//This is the lax reorder buffer
//Lax means 'inexact' behaviour, i.e. retiring holes (empty ROB slots) is
//supported and any stragglers will be retired out-of-order

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct p64_laxrob p64_laxrob_t;

typedef struct p64_laxrob_elem
{
    struct p64_laxrob_elem *next;
    uint32_t sn;
} p64_laxrob_elem_t;

//Call-back signature, 'elem' is a list of elements
//The call-back is invoked with a NULL elem pointer in order to signal
//the flushing of any buffered output
typedef void (*p64_laxrob_cb)(void *arg, p64_laxrob_elem_t *elem);

//Allocate a reorder buffer with space for at least 'nelems' elements
p64_laxrob_t *p64_laxrob_alloc(uint32_t nelems,
			       p64_laxrob_cb cb,
			       void *arg);

//Free a reorder buffer
//The reorder buffer must be empty
void p64_laxrob_free(p64_laxrob_t *rb);

//Insert list of elements into the reorder buffer
//Retire any in-order elements and invoke the callback
void p64_laxrob_insert(p64_laxrob_t *rb,
		       p64_laxrob_elem_t *elem);

//Retire any in-order elements and invoke the callback
void p64_laxrob_flush(p64_laxrob_t *rb,
		      uint32_t nelems);

#ifdef __cplusplus
}
#endif

#endif
