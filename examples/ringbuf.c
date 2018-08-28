//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>

#include "p64_ringbuf.h"
#include "expect.h"

#define ELEM(x) ((void *)(x))

static void
test_rb(uint32_t flags)
{
    void *vec[4];
    int ret;
    uint32_t index;

    p64_ringbuf_t *rb = p64_ringbuf_alloc(1, flags);
    EXPECT(rb != NULL);

    ret = p64_ringbuf_dequeue(rb, vec, 1, &index);
    EXPECT(ret == 0);
    ret = p64_ringbuf_enqueue(rb, (void *[]){ ELEM(1) }, 1);
    EXPECT(ret == 1);

    ret = p64_ringbuf_dequeue(rb, vec, 1, &index);
    EXPECT(ret == 1);
    EXPECT(index == 0);
    EXPECT(vec[0] == ELEM(1));

    ret = p64_ringbuf_dequeue(rb, vec, 1, &index);
    EXPECT(ret == 0);

    ret = p64_ringbuf_enqueue(rb, (void *[]){ ELEM(2), ELEM(3), ELEM(4) }, 3);
    EXPECT(ret == 2);

    ret = p64_ringbuf_dequeue(rb, vec, 1, &index);
    EXPECT(ret == 1);
    EXPECT(index == 1);
    EXPECT(vec[0] == ELEM(2));

    ret = p64_ringbuf_dequeue(rb, vec, 4, &index);
    EXPECT(ret == 1);
    EXPECT(index == 2);
    EXPECT(vec[0] == ELEM(3));

    p64_ringbuf_free(rb);
}

int main(void)
{
    printf("testing MPMC ring buffer\n");
    test_rb(P64_RINGBUF_F_MPENQ | P64_RINGBUF_F_MCDEQ);
    printf("testing SPSC ring buffer\n");
    test_rb(P64_RINGBUF_F_SPENQ | P64_RINGBUF_F_SCDEQ);
    printf("testing LFC ring buffer\n");
    test_rb(P64_RINGBUF_F_LFDEQ);
    printf("ringbuf test complete\n");
    return 0;
}
