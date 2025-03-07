//Copyright (c) 2025, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

//A lock-free double ended queue (deque)
//See "CAS-Based Lock-Free Algorithm for Shared Deques" by Michael

#ifndef P64_DEQUE_H
#define P64_DEQUE_H

#ifdef __cplusplus
extern "C"
{
#endif

//Include this struct (first) in a user-defined struct
typedef struct p64_deque_elem
{
    struct p64_deque_elem *elem[2];
} p64_deque_elem_t;

typedef struct
{
    _Alignas(2 * sizeof(void *))
    p64_deque_elem_t *end[2];
} p64_deque_t;

void p64_deque_init(p64_deque_t *deq);

//Enqueue element onto left or right end
void p64_deque_enqueue_l(p64_deque_t *deq, p64_deque_elem_t *elem);
void p64_deque_enqueue_r(p64_deque_t *deq, p64_deque_elem_t *elem);

//Dequeue element from left or right end
//A dequeued element must not be re-enqueued before it has been retired and reclaimed
//Use e.g. QSBR to perform safe memory reclamation
p64_deque_elem_t *p64_deque_dequeue_l(p64_deque_t *deq);
p64_deque_elem_t *p64_deque_dequeue_r(p64_deque_t *deq);

#ifdef __cplusplus
}
#endif

#endif
