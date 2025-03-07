//Copyright (c) 2025, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "p64_deque.h"
#include "expect.h"
#include "os_abstraction.h"

struct element
{
    p64_deque_elem_t elem;
    uint32_t data;
};

static struct element *
elem_alloc(uint32_t data)
{
    struct element *elem;
    elem = malloc(sizeof(struct element));
    if (elem == NULL)
    {
	perror("malloc"), exit(-1);
    }
    elem->data = data;
    return elem;
}

static void
test_deq(void)
{
    p64_deque_t deq;
    struct element *elem;

    p64_deque_init(&deq);
    elem = (struct element *)p64_deque_dequeue_l(&deq);
    EXPECT(elem == NULL);
    elem = (struct element *)p64_deque_dequeue_r(&deq);
    EXPECT(elem == NULL);
    elem = elem_alloc(10);
    p64_deque_enqueue_r(&deq, &elem->elem);
    elem = (struct element *)p64_deque_dequeue_l(&deq);
    EXPECT(elem != NULL && elem->data == 10);
    free(elem);
    elem = (struct element *)p64_deque_dequeue_r(&deq);
    EXPECT(elem == NULL);
    elem = elem_alloc(30);
    p64_deque_enqueue_r(&deq, &elem->elem);
    elem = elem_alloc(20);
    p64_deque_enqueue_l(&deq, &elem->elem);
    elem = elem_alloc(40);
    p64_deque_enqueue_r(&deq, &elem->elem);
    elem = (struct element *)p64_deque_dequeue_l(&deq);
    EXPECT(elem != NULL && elem->data == 20);
    free(elem);
    elem = (struct element *)p64_deque_dequeue_l(&deq);
    EXPECT(elem != NULL && elem->data == 30);
    free(elem);
    elem = (struct element *)p64_deque_dequeue_l(&deq);
    EXPECT(elem != NULL && elem->data == 40);
    free(elem);
    elem = (struct element *)p64_deque_dequeue_l(&deq);
    EXPECT(elem == NULL);
}

int main(void)
{
    printf("testing deque\n");
    test_deq();
    return 0;
}
