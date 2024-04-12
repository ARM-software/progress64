//Copyright (c) 2024, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

//Treiber lock-free stack (using tagged pointers) with backoff and
//progress-in-update flag for enhanced scalability when using atomics

#ifndef P64_LFSTACK_H
#define P64_LFSTACK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct p64_lfstack_elem
{
    struct p64_lfstack_elem *next;
} p64_lfstack_elem_t;

typedef struct p64_lfstack
{
    _Alignas(2 * sizeof(void *))
    p64_lfstack_elem_t *head;
    uintptr_t tag;
} p64_lfstack_t;

//Initialise a lock-free stack
void
p64_lfstack_init(p64_lfstack_t *stk);

//Enqueue (push) an element to the stack
void
p64_lfstack_enqueue(p64_lfstack_t *stk, p64_lfstack_elem_t *elem);

//Dequeue (pop) an element from the stack
p64_lfstack_elem_t *
p64_lfstack_dequeue(p64_lfstack_t *stk);

#ifdef __cplusplus
}
#endif

#endif
