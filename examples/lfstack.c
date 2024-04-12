//Copyright (c) 2024, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "p64_errhnd.h"
#include "p64_lfstack.h"
#include "expect.h"

#define ERR_NULL_ELEM 1

static jmp_buf jmpbuf;

static int
error_handler(const char *module, const char *cur_err, uintptr_t val)
{
    (void)val;
    EXPECT(strcmp(module, "lfstack") == 0);
    if (strcmp(cur_err, "enqueue NULL element") == 0)
    {
	longjmp(jmpbuf, ERR_NULL_ELEM);
    }
    fprintf(stderr, "lfstack: unexpected error reported: %s\n", cur_err);
    fflush(NULL);
    abort();
}

struct my_elem
{
    p64_lfstack_elem_t *next;
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
test_stk(void)
{
    p64_lfstack_t stk;
    struct my_elem *elem;
    int jv;
    p64_errhnd_install(error_handler);

    p64_lfstack_init(&stk);
    elem = (void *)p64_lfstack_dequeue(&stk);
    EXPECT(elem == NULL);
    p64_lfstack_enqueue(&stk, (void *)elem_alloc(10));
    elem = (void *)p64_lfstack_dequeue(&stk);
    EXPECT(elem != NULL && elem->key == 10);
    free(elem);
    //Check that enqueue of (invalid) NULL pointer is detected
    if ((jv = setjmp(jmpbuf)) == 0)
    {
	p64_lfstack_enqueue(&stk, NULL);
	EXPECT(!"p64_lfstack_enqueue() unexpectedly succeeded");
    }
    //Else longjumped back from error handler
    EXPECT(jv == ERR_NULL_ELEM);
    elem = (void *)p64_lfstack_dequeue(&stk);
    EXPECT(elem == NULL);
    p64_lfstack_enqueue(&stk, (void *)elem_alloc(20));
    p64_lfstack_enqueue(&stk, (void *)elem_alloc(30));
    elem = (void *)p64_lfstack_dequeue(&stk);
    EXPECT(elem != NULL && elem->key == 30);
    free(elem);
    elem = (void *)p64_lfstack_dequeue(&stk);
    EXPECT(elem != NULL && elem->key == 20);
    free(elem);
    elem = (void *)p64_lfstack_dequeue(&stk);
    EXPECT(elem == NULL);
}

int main(void)
{
    printf("testing lock-free stack\n");
    test_stk();
    printf("stack test complete\n");
    return 0;
}
