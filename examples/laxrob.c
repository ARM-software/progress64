//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdio.h>
#include "p64_laxrob.h"
#include "expect.h"

static uint32_t nretired = 0;
static uint32_t last_sn = 0;

static void
callback(void *arg, p64_laxrob_elem_t **vec, uint32_t nitems)
{
    (void)arg;
    for (uint32_t i = 0; i < nitems; i++)
    {
	p64_laxrob_elem_t *elem = vec[i];
	EXPECT(elem->next == NULL);
	printf("Element %u retired\n", elem->sn);
	nretired++;
	last_sn = elem->sn;
	free(elem);
    }
}

static p64_laxrob_elem_t *
alloc_elem(uint32_t sn)
{
    p64_laxrob_elem_t *elem = malloc(sizeof(p64_laxrob_elem_t));
    if (elem == NULL)
    {
	perror("malloc"), exit(EXIT_FAILURE);
    }
    elem->next = NULL;
    elem->sn = sn;
    return elem;
}

int main(void)
{
    p64_laxrob_t *rb = p64_laxrob_alloc(4, 1, callback, NULL);
    EXPECT(rb != NULL);
    printf("Insert 0\n");
    p64_laxrob_insert(rb, alloc_elem(0));
    printf("Insert 0\n");
    p64_laxrob_insert(rb, alloc_elem(0));
    printf("Flush\n");
    p64_laxrob_flush(rb, 1);
    EXPECT(nretired == 2);
    EXPECT(last_sn == 0);
    printf("Insert 2\n");
    p64_laxrob_insert(rb, alloc_elem(2));
    printf("Insert 2\n");
    p64_laxrob_insert(rb, alloc_elem(2));
    EXPECT(nretired == 2);
    EXPECT(last_sn == 0);
    printf("Insert 1\n");
    p64_laxrob_insert(rb, alloc_elem(1));
    EXPECT(nretired == 2);
    EXPECT(last_sn == 0);
    printf("Insert 5\n");
    p64_laxrob_insert(rb, alloc_elem(5));
    EXPECT(nretired == 3);
    EXPECT(last_sn == 1);
    printf("Flush\n");
    p64_laxrob_flush(rb, 4);
    EXPECT(nretired == 6);
    EXPECT(last_sn == 5);
    p64_laxrob_free(rb);

    printf("laxrob tests complete\n");
    return 0;
}
