//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>

#include "p64_lfring.h"

#include "expect.h"

static void
test_rb(uint32_t flags)
{
    void *vec[4];
    uint32_t ret;
    uint32_t idx;

    p64_lfring_t *rb = p64_lfring_alloc(2, flags);
    EXPECT(rb != NULL);

    ret = p64_lfring_dequeue(rb, vec, 1, &idx);
    EXPECT(ret == 0);
    ret = p64_lfring_enqueue(rb, (void *[]){ (void*)1 }, 1);
    EXPECT(ret == 1);

    ret = p64_lfring_dequeue(rb, vec, 1, &idx);
    EXPECT(ret == 1);
    EXPECT(idx == 0);
    EXPECT(vec[0] == (void*)1);

    ret = p64_lfring_dequeue(rb, vec, 1, &idx);
    EXPECT(ret == 0);

    ret = p64_lfring_enqueue(rb, (void *[]){ (void*)2, (void*)3, (void*)4 }, 3);
    EXPECT(ret == 2);

    ret = p64_lfring_dequeue(rb, vec, 1, &idx);
    EXPECT(ret == 1);
    EXPECT(idx == 1);
    EXPECT(vec[0] == (void*)2);

    ret = p64_lfring_dequeue(rb, vec, 4, &idx);
    EXPECT(ret == 1);
    EXPECT(idx == 2);
    EXPECT(vec[0] == (void*)3);

    p64_lfring_free(rb);
}

int main(void)
{
    printf("testing MPMC lock-free ring\n");
    test_rb(P64_LFRING_F_MPENQ | P64_LFRING_F_MCDEQ);
    printf("testing MPSC lock-free ring\n");
    test_rb(P64_LFRING_F_MPENQ | P64_LFRING_F_SCDEQ);
    printf("testing SPMC lock-free ring\n");
    test_rb(P64_LFRING_F_SPENQ | P64_LFRING_F_MCDEQ);
    printf("testing SPSC lock-free ring\n");
    test_rb(P64_LFRING_F_SPENQ | P64_LFRING_F_SCDEQ);
    printf("lock-free ring tests complete\n");
    return 0;
}
