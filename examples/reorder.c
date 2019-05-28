//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdio.h>
#include "p64_reorder.h"
#include "expect.h"

static uint32_t next_elem = 100;

static void callback(void *arg, void *elem, uint32_t sn)
{
    (void)arg;
    EXPECT(elem != P64_REORDER_DUMMY);
    if (elem != NULL)
    {
	printf("Element %lu retired\n", (uintptr_t)elem);
	EXPECT((uintptr_t)elem == next_elem);
	EXPECT(sn + 100 == next_elem);
	next_elem++;
    }
    else
    {
	EXPECT(sn + 100 == next_elem);
    }
}

int main(void)
{
    uint32_t sn;
    p64_reorder_t *rob = p64_reorder_alloc(4, false, callback, NULL);
    EXPECT(rob != NULL);
    EXPECT(p64_reorder_acquire(rob, 1, &sn) == 1);
    EXPECT(sn == 0);
    EXPECT(p64_reorder_acquire(rob, 2, &sn) == 2);
    EXPECT(sn == 1);
    EXPECT(p64_reorder_acquire(rob, 1, &sn) == 1);
    EXPECT(sn == 3);
    EXPECT(p64_reorder_acquire(rob, 1, &sn) == 0);
    p64_reorder_release(rob, 3, &(void *){(void*)103}, 1);
    EXPECT(p64_reorder_acquire(rob, 1, &sn) == 0);
    p64_reorder_release(rob, 0, &(void *){(void*)100}, 1);
    EXPECT(p64_reorder_acquire(rob, 1, &sn) == 1);
    EXPECT(sn == 4);
    EXPECT(p64_reorder_acquire(rob, 1, &sn) == 0);
    p64_reorder_release(rob, 2, &(void *){(void*)102}, 1);
    p64_reorder_release(rob, 1, &(void *){(void*)101}, 1);
    p64_reorder_release(rob, 4, &(void *){(void*)104}, 1);
    p64_reorder_free(rob);

    printf("reorder tests complete\n");
    return 0;
}
