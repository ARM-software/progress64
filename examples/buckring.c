//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>

#include "p64_buckring.h"
#include "expect.h"

//Must leave 2lsb unused (zero)
#define ONE (void *)(1<<2)
#define TWO (void *)(2<<2)
#define THREE (void *)(3<<2)
#define FOUR (void *)(4<<2)

static void
test_rb(void)
{
    void *vec[4];
    int ret;
    uint32_t index;

    p64_buckring_t *rb = p64_buckring_alloc(2, 0);
    EXPECT(rb != NULL);

    ret = p64_buckring_dequeue(rb, vec, 1, &index);
    EXPECT(ret == 0);
    ret = p64_buckring_enqueue(rb, (void *[]){ ONE }, 1);
    EXPECT(ret == 1);

    ret = p64_buckring_dequeue(rb, vec, 1, &index);
    EXPECT(ret == 1);
    EXPECT(index == 0);
    EXPECT(vec[0] == ONE);

    ret = p64_buckring_dequeue(rb, vec, 1, &index);
    EXPECT(ret == 0);

    ret = p64_buckring_enqueue(rb, (void *[]){ TWO, THREE, FOUR }, 3);
    EXPECT(ret == 2);

    ret = p64_buckring_dequeue(rb, vec, 1, &index);
    EXPECT(ret == 1);
    EXPECT(index == 1);
    EXPECT(vec[0] == TWO);

    ret = p64_buckring_dequeue(rb, vec, 4, &index);
    EXPECT(ret == 1);
    EXPECT(index == 2);
    EXPECT(vec[0] == THREE);

    p64_buckring_free(rb);
}

int main(void)
{
    printf("testing buckring\n");
    test_rb();
    printf("buckring test complete\n");
    return 0;
}
