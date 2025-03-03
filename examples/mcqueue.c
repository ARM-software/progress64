//Copyright (c) 2025, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "p64_mcqueue.h"
#include "expect.h"
#include "os_abstraction.h"

struct element
{
    p64_mcqueue_elem_t elem;
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
test_mcq(void)
{
    p64_mcqueue_t mcq;
    struct element *elem;

    p64_mcqueue_init(&mcq);
    elem = (struct element *)p64_mcqueue_dequeue(&mcq);
    EXPECT(elem == NULL);
    elem = elem_alloc(10);
    p64_mcqueue_enqueue(&mcq, &elem->elem);
    elem = (struct element *)p64_mcqueue_dequeue(&mcq);
    EXPECT(elem != NULL && elem->data == 10);
    free(elem);
    elem = (struct element *)p64_mcqueue_dequeue(&mcq);
    EXPECT(elem == NULL);
    elem = elem_alloc(20);
    p64_mcqueue_enqueue(&mcq, &elem->elem);
    elem = elem_alloc(30);
    p64_mcqueue_enqueue(&mcq, &elem->elem);
    elem = elem_alloc(40);
    p64_mcqueue_enqueue(&mcq, &elem->elem);
    elem = (struct element *)p64_mcqueue_dequeue(&mcq);
    EXPECT(elem != NULL && elem->data == 20);
    free(elem);
    elem = (struct element *)p64_mcqueue_dequeue(&mcq);
    EXPECT(elem != NULL && elem->data == 30);
    free(elem);
    elem = (struct element *)p64_mcqueue_dequeue(&mcq);
    EXPECT(elem != NULL && elem->data == 40);
    free(elem);
    elem = (struct element *)p64_mcqueue_dequeue(&mcq);
    EXPECT(elem == NULL);
}

int main(void)
{
    printf("testing mcqueue\n");
    test_mcq();
    return 0;
}
