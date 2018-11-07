//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>

#include "p64_lfring.h"

#include "expect.h"

static void
test_rb(void)
{
    void *vec[4];
    int ret;

    p64_lfring_t *rb = p64_lfring_alloc(1);
    EXPECT(rb != NULL);

    ret = p64_lfring_dequeue(rb, vec, 1);
    EXPECT(ret == 0);
    ret = p64_lfring_enqueue(rb, (void *[]){ (void*)1 }, 1);
    EXPECT(ret == 1);

    ret = p64_lfring_dequeue(rb, vec, 1);
    EXPECT(ret == 1);
    EXPECT(vec[0] == (void*)1);

    ret = p64_lfring_dequeue(rb, vec, 1);
    EXPECT(ret == 0);

    ret = p64_lfring_enqueue(rb, (void *[]){ (void*)2, (void*)3, (void*)4 }, 3);
    EXPECT(ret == 2);

    ret = p64_lfring_dequeue(rb, vec, 1);
    EXPECT(ret == 1);
    EXPECT(vec[0] == (void*)2);

    ret = p64_lfring_dequeue(rb, vec, 4);
    EXPECT(ret == 1);
    EXPECT(vec[0] == (void*)3);

    p64_lfring_free(rb);
}

int main(void)
{
    printf("testing lock-free ring\n");
    test_rb();
    printf("lock-free ring test complete\n");
    return 0;
}
