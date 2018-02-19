//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdio.h>
#include "p64_reorder.h"
#include "expect.h"

static uint32_t next_sn = 100;

static void callback(void *arg, void *elem)
{
    printf("Element %lu retired\n", (uintptr_t)elem);
    EXPECT((uintptr_t)elem == next_sn);
    next_sn++;
}

int main(void)
{
    uint32_t sn;
    p64_reorder_t *rb = p64_reorder_alloc(4, callback, NULL);
    EXPECT(rb != NULL);
    EXPECT(p64_reorder_reserve(rb, 1, &sn) == 1);
    EXPECT(sn == 0);
    EXPECT(p64_reorder_reserve(rb, 2, &sn) == 2);
    EXPECT(sn == 1);
    EXPECT(p64_reorder_reserve(rb, 1, &sn) == 1);
    EXPECT(sn == 3);
    EXPECT(p64_reorder_reserve(rb, 1, &sn) == 0);
    p64_reorder_insert(rb, 1, &(void *){(void*)103}, 3);
    EXPECT(p64_reorder_reserve(rb, 1, &sn) == 0);
    p64_reorder_insert(rb, 1, &(void *){(void*)100}, 0);
    EXPECT(p64_reorder_reserve(rb, 1, &sn) == 1);
    EXPECT(sn == 4);
    EXPECT(p64_reorder_reserve(rb, 1, &sn) == 0);
    p64_reorder_insert(rb, 1, &(void *){(void*)102}, 2);
    p64_reorder_insert(rb, 1, &(void *){(void*)101}, 1);
    p64_reorder_insert(rb, 1, &(void *){(void*)104}, 4);
    p64_reorder_free(rb);

    printf("reorder tests complete\n");
    return 0;
}
