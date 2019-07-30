//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

//Treiber lock-free stack
//Different ABA workarounds are supported: lock, tagged pointers, safe memory
//reclamation (SMR) and LL/SC (exclusives on Arm)

#ifndef _P64_STACK_H
#define _P64_STACK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

//ABA workaround methods
#define P64_ABA_LOCK 0x0000 //Use spin lock for mutual exclusion
#define P64_ABA_TAG  0x0001 //Requires double-word (lockfree16) CAS
#define P64_ABA_SMR  0x0002 //Use hazard pointer API
#define P64_ABA_LLSC 0x0003 //Requires LLSC support (e.g. Arm but not x86)

typedef struct p64_stack_elem
{
    struct p64_stack_elem *next;
} p64_stack_elem_t;

typedef struct p64_stack
{
    _Alignas(2 * sizeof(void *))
    p64_stack_elem_t *head;
    uintptr_t tag;
} p64_stack_t;

//Initialise a Treiber lock-free stack
void
p64_stack_init(p64_stack_t *stk, uint32_t aba_workaround);

//Enqueue (push) an element to the stack
//With SMR as ABA workaround, LIFO order is not guaranteed
void
p64_stack_enqueue(p64_stack_t *stk, p64_stack_elem_t *elem);

//Dequeue (pop) an element from the stack
p64_stack_elem_t *
p64_stack_dequeue(p64_stack_t *stk);

#ifdef __cplusplus
}
#endif

#endif
