//Copyright (c) 2025, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include "p64_mcqueue.h"
#include "atomic.h"

void
p64_mcqueue_init(p64_mcqueue_t *queue)
{
    queue->head = NULL;
    queue->tail = NULL;
}

//Enqueue at tail
void
p64_mcqueue_enqueue(p64_mcqueue_t *queue,
		    p64_mcqueue_elem_t *elem)
{
    elem->next = NULL;
    p64_mcqueue_elem_t *last = atomic_exchange_ptr(&queue->tail, elem, __ATOMIC_RELAXED);
    if (last == NULL)
    {
	//A1: Synchronize-with A0
	atomic_store_ptr(&queue->head, elem, __ATOMIC_RELEASE);
    }
    else
    {
	//B1: Synchronize-with B0
	atomic_store_ptr(&last->next, elem, __ATOMIC_RELEASE);
    }
}

//Dequeue from head
p64_mcqueue_elem_t *
p64_mcqueue_dequeue(p64_mcqueue_t *queue)
{
    p64_mcqueue_elem_t *first;
    for (;;)
    {
	//A0: Synchronize-with A1/A2
	first = atomic_exchange_ptr(&queue->head, NULL, __ATOMIC_ACQUIRE);
	if (first != NULL)
	{
	    //Got an element
	    break;
	}
	else if (atomic_load_ptr(&queue->tail, __ATOMIC_RELAXED) == NULL)
	{
	    //No element at either head or tail => queue is empty
	    return NULL;
	}
	//Must yield in loop when verifying
	VERIFY_YIELD();
    }
    //B0: Synchronize-with B1
    p64_mcqueue_elem_t *second = atomic_load_ptr(&first->next, __ATOMIC_ACQUIRE);
    if (second == NULL)
    {
	//'first' was the only element in the queue, need to update tail as well
	p64_mcqueue_elem_t *copy = first;//Use a copy
	//Only update tail if it still points to 'first'
	if (!atomic_compare_exchange_ptr(&queue->tail, &copy, NULL, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
	{
	    //'first' is no longer at tail
	    //Wait for concurrent enqueue to complete
	    wait_until_not_equal_ptr(&first->next, NULL, __ATOMIC_RELAXED);
	    atomic_store_ptr(&queue->head, first->next, __ATOMIC_RELAXED);
	}
    }
    else
    {
	//Make 'second' the new head element of the queue
	//A2: Synchronize-with A0
	atomic_store_ptr(&queue->head, second, __ATOMIC_RELEASE);
    }
    //Return the element that was at the head of the queue
    return first;
}
