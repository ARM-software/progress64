//Copyright (c) 2025, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

//Mellor-Crummey concurrent queue. It is not non-blocking/lock-free.
//See "Concurrent Queues - Practical Fetch-and-Φ Algorithms" by Mellor-Crummey

#ifndef P64_MCQUEUE_H
#define P64_MCQUEUE_H

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct p64_mcqueue_elem
{
    struct p64_mcqueue_elem *next;
} p64_mcqueue_elem_t;

typedef struct
{
    p64_mcqueue_elem_t *head;
    p64_mcqueue_elem_t *tail;
} p64_mcqueue_t;

//Initialise Melloc-Crummey queue
void
p64_mcqueue_init(p64_mcqueue_t *queue);

//Enqueue (push) an element to the mcqueue
void
p64_mcqueue_enqueue(p64_mcqueue_t *queue,
		    p64_mcqueue_elem_t *elem);

//Dequeue (pop) an element from the mcqueue
p64_mcqueue_elem_t *
p64_mcqueue_dequeue(p64_mcqueue_t *queue);

#ifdef __cplusplus
}
#endif

#endif
