//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

//'Lax' reorder buffer
//Lax means 'inexact' behaviour, i.e. retiring holes (empty ROB slots) is
//supported and any stragglers will be retired out-of-order

#ifndef P64_LAXROB_H
#define P64_LAXROB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct p64_laxrob p64_laxrob_t;

//p64_laxrob_elem_t has the same structure as DPDK struct rte_mbuf
typedef struct p64_laxrob_elem
{
    char pad_line[64];
    struct p64_laxrob_elem *next;//Linked list next pointer
    char pad[36 - 8];//Gap between rte_mbuf userdata and seqn
    uint32_t sn;
} p64_laxrob_elem_t;

//Call-back signature, 'vec' is an array (size 'n') of pointers to elements
typedef void (*p64_laxrob_cb)(void *arg, p64_laxrob_elem_t **vec, uint32_t n);

//Allocate a reorder buffer with space for at least 'nslots' slots
p64_laxrob_t *p64_laxrob_alloc(uint32_t nslots,
			       uint32_t vecsz,
			       p64_laxrob_cb cb,
			       void *arg);

//Free a reorder buffer
//The reorder buffer must be empty
void p64_laxrob_free(p64_laxrob_t *rb);

//Insert list of elements into the reorder buffer
//If necessary, retire in-order elements and invoke the callback
void p64_laxrob_insert(p64_laxrob_t *rb,
		       p64_laxrob_elem_t *elem);

//Retire any in-order elements and invoke the callback
void p64_laxrob_flush(p64_laxrob_t *rb,
		      uint32_t nslots);

#ifdef __cplusplus
}
#endif

#endif
