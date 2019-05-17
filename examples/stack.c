//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>

#include "p64_hazardptr.h"
#include "p64_stack.h"
#include "expect.h"

#define NUM_HAZARD_POINTERS 1

struct my_elem
{
    p64_stack_elem_t *next;
    uint32_t key;
};

static struct my_elem *
elem_alloc(uint32_t k)
{
    struct my_elem *he = malloc(sizeof(struct my_elem));
    if (he == NULL)
        perror("malloc"), exit(-1);
    he->next = NULL;
    he->key = k;
    return he;
}

static void
test_stk(uint32_t flags)
{
    p64_stack_t stk;
    struct my_elem *elem;
    p64_hpdomain_t *hpd = NULL;

    if (flags == P64_ABA_SMR)
    {
	hpd = p64_hazptr_alloc(10, NUM_HAZARD_POINTERS);
	EXPECT(hpd != NULL);
	p64_hazptr_register(hpd);
    }

    p64_stack_init(&stk, flags);
    elem = (void *)p64_stack_dequeue(&stk);
    EXPECT(elem == NULL);
    p64_stack_enqueue(&stk, (void *)elem_alloc(10));
    elem = (void *)p64_stack_dequeue(&stk);
    EXPECT(elem != NULL && elem->key == 10);
    free(elem);
    elem = (void *)p64_stack_dequeue(&stk);
    EXPECT(elem == NULL);
    p64_stack_enqueue(&stk, (void *)elem_alloc(20));
    p64_stack_enqueue(&stk, (void *)elem_alloc(30));
    elem = (void *)p64_stack_dequeue(&stk);
    EXPECT(elem != NULL && elem->key == 30);
    free(elem);
    elem = (void *)p64_stack_dequeue(&stk);
    EXPECT(elem != NULL && elem->key == 20);
    free(elem);
    elem = (void *)p64_stack_dequeue(&stk);
    EXPECT(elem == NULL);

    if (flags == P64_ABA_SMR)
    {
	p64_hazptr_unregister();
	p64_hazptr_free(hpd);
    }
}

int main(void)
{
    printf("testing lock-based stack\n");
    test_stk(P64_ABA_LOCK);
    printf("testing tag-based stack\n");
    test_stk(P64_ABA_TAG);
    printf("testing smr-based stack\n");
    test_stk(P64_ABA_SMR);
#ifdef __aarch64__
    printf("testing llsc-based stack\n");
    test_stk(P64_ABA_LLSC);
#endif
    printf("stack test complete\n");
    return 0;
}
