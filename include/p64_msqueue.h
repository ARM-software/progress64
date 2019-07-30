//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

//Michael & Scott lock-free queue
//Different ABA workarounds are supported: locks (head & tail), tagged pointers
//and safe memory reclamation (SMR)

#ifndef _P64_MSQUEUE_H
#define _P64_MSQUEUE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

//ABA workaround methods
#define P64_ABA_LOCK 0x0000 //Use spin locks for mutual exclusion
#define P64_ABA_TAG  0x0001 //Requires double-word (lockfree16) CAS
#define P64_ABA_SMR  0x0002 //Use hazard pointer API

typedef struct p64_ptr_tag
{
    _Alignas(2 * sizeof(void *))
    struct p64_msqueue_elem *ptr;
    uintptr_t tag;
} p64_ptr_tag_t;

typedef struct p64_msqueue_elem
{
    struct p64_ptr_tag next;
    size_t user_len;
    char user[];
} p64_msqueue_elem_t;

//Initialise Michael&Scott lock-free queue
//A dynamically allocated dummy element must be specified
void
p64_msqueue_init(p64_ptr_tag_t *qhead, p64_ptr_tag_t *qtail,
		 uint32_t aba_workaround,
		 p64_msqueue_elem_t *dummy);

//Finish Michael&Scott queue
//Return (a potentially different) dummy element to the user
p64_msqueue_elem_t *
p64_msqueue_fini(p64_ptr_tag_t *qhead, p64_ptr_tag_t *qtail);

//Enqueue (push) an element to the msqueue
void
p64_msqueue_enqueue(p64_ptr_tag_t *qhead, p64_ptr_tag_t *qtail,
		    p64_msqueue_elem_t *elem);

//Dequeue (pop) an element from the msqueue
p64_msqueue_elem_t *
p64_msqueue_dequeue(p64_ptr_tag_t *qhead, p64_ptr_tag_t *qtail);

#ifdef __cplusplus
}
#endif

#endif
