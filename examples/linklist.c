//Copyright (c) 2025, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "p64_errhnd.h"
#include "p64_linklist.h"
#include "expect.h"

#define ERR_NULL_ELEM 1

static jmp_buf jmpbuf;

static int
error_handler(const char *module, const char *cur_err, uintptr_t val)
{
    (void)val;
    EXPECT(strcmp(module, "linklist") == 0);
    if (strcmp(cur_err, "insert NULL element") == 0)
    {
	longjmp(jmpbuf, ERR_NULL_ELEM);
    }
    fprintf(stderr, "linklist: unexpected error reported: %s\n", cur_err);
    fflush(NULL);
    abort();
}

struct my_elem
{
    p64_linklist_t elem;
    uint32_t key;
};

static struct my_elem *
elem_alloc(uint32_t k)
{
    struct my_elem *me = malloc(sizeof(struct my_elem));
    if (me == NULL)
	perror("malloc"), exit(-1);
    me->elem.next = NULL;
    me->key = k;
    return me;
}

#define container_of(pointer, type, member) \
    ((type *)(void *)(((char *)pointer) - offsetof(type, member)))

static p64_linklist_t *
lookup(p64_linklist_t *list, uint32_t key)
{
    p64_linklist_t *curr = list;
    while ((curr = p64_linklist_next(curr)) != NULL)
    {
	const struct my_elem *obj = container_of(curr, struct my_elem, elem);
	if (obj->key == key)
	{
	    return curr;
	}
    }
    //Element not found, someone else might have removed it
    return NULL;
}


static void
test_list(void)
{
    p64_linklist_t list;
    struct my_elem *me1, *me2;
    p64_linklist_t *elem;
    int jv;

    p64_errhnd_install(error_handler);
    p64_linklist_init(&list);
    me1 = elem_alloc(10);
    //Insert me1 first in list
    EXPECT(p64_linklist_insert(&list, &me1->elem) == true);
    me2 = elem_alloc(20);
    //Insert me2 after me1
    EXPECT(p64_linklist_insert(&me1->elem, &me2->elem) == true);
    //Remove me1; me2 is now first
    EXPECT(p64_linklist_remove(&list, &me1->elem) == true);
    elem = lookup(&list, 20);
    EXPECT(elem == &me2->elem);
    //Attempt to remove me1 again
    EXPECT(p64_linklist_remove(&list, &me1->elem) == true);
    elem = lookup(&list, 20);
    EXPECT(elem == &me2->elem);
    //Remove me2
    EXPECT(p64_linklist_remove(&list, &me2->elem) == true);
    elem = lookup(&list, 20);
    EXPECT(elem == NULL);
    //Check that insert of (invalid) NULL pointer is detected
    if ((jv = setjmp(jmpbuf)) == 0)
    {
	p64_linklist_insert(&list, NULL);
	EXPECT(!"p64_linklist_insert() unexpectedly succeeded");
    }
    //Else longjumped back from error handler
    EXPECT(jv == ERR_NULL_ELEM);
    free(me1);
    free(me2);
}

int main(void)
{
    printf("testing (lock-free) linked list\n");
    test_list();
    printf("linked list test complete\n");
    return 0;
}
