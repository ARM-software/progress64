//Copyright (c) 2024, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <inttypes.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "p64_blkring.h"

#include "expect.h"

static void
test_rb(void)
{
    void *vec[4];
    uint32_t index;

    p64_blkring_t *rb = p64_blkring_alloc(5);
    EXPECT(rb != NULL);

    p64_blkring_enqueue(rb, (void *[]){ (void *)1 }, 1);
    p64_blkring_dequeue(rb, vec, 1, &index);
    EXPECT(index == 0);
    EXPECT(vec[0] == (void *)1);

    p64_blkring_enqueue(rb, (void *[]){ (void *)2, (void *)3, (void *)4 , (void *)5, (void *)6 }, 5);
    p64_blkring_dequeue(rb, vec, 1, &index);
    EXPECT(index == 1);
    EXPECT(vec[0] == (void *)2);

    p64_blkring_dequeue(rb, vec, 2, &index);
    EXPECT(index == 2);
    EXPECT(vec[0] == (void *)3);
    EXPECT(vec[1] == (void *)4);

    int32_t ret = p64_blkring_dequeue_nblk(rb, vec, 3, &index);
    EXPECT(ret == 2);
    EXPECT(index == 4);
    EXPECT(vec[0] == (void *)5);
    EXPECT(vec[1] == (void *)6);

    p64_blkring_free(rb);
}

int main(void)
{
    printf("testing blocking ring buffer\n");
    test_rb();
    printf("blkring test complete\n");
    return 0;
}
