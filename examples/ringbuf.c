//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>

#include "p64_ringbuf_template.h"
//Instantiate the ring buffer template using uint32_t as the ring element
P64_RINGBUF(p64_ringbuf_ui32, uint32_t)

#include "expect.h"

static void
test_rb(uint32_t flags)
{
    uint32_t vec[4];
    int ret;
    uint32_t index;

    p64_ringbuf_ui32_t *rb = p64_ringbuf_ui32_alloc(2, flags);
    EXPECT(rb != NULL);

    ret = p64_ringbuf_ui32_dequeue(rb, vec, 1, &index);
    EXPECT(ret == 0);
    ret = p64_ringbuf_ui32_enqueue(rb, (uint32_t[]){ 1 }, 1);
    EXPECT(ret == 1);

    ret = p64_ringbuf_ui32_dequeue(rb, vec, 1, &index);
    EXPECT(ret == 1);
    EXPECT(index == 0);
    EXPECT(vec[0] == 1);

    ret = p64_ringbuf_ui32_dequeue(rb, vec, 1, &index);
    EXPECT(ret == 0);

    ret = p64_ringbuf_ui32_enqueue(rb, (uint32_t[]){ 2, 3, 4 }, 3);
    EXPECT(ret == 2);

    ret = p64_ringbuf_ui32_dequeue(rb, vec, 1, &index);
    EXPECT(ret == 1);
    EXPECT(index == 1);
    EXPECT(vec[0] == 2);

    ret = p64_ringbuf_ui32_dequeue(rb, vec, 4, &index);
    EXPECT(ret == 1);
    EXPECT(index == 2);
    EXPECT(vec[0] == 3);

    p64_ringbuf_ui32_free(rb);
}

int main(void)
{
    printf("testing MPMC ring buffer\n");
    test_rb(P64_RINGBUF_F_MPENQ | P64_RINGBUF_F_MCDEQ);
    printf("testing SPSC ring buffer\n");
    test_rb(P64_RINGBUF_F_SPENQ | P64_RINGBUF_F_SCDEQ);
    printf("testing MPLFC ring buffer\n");
    test_rb(P64_RINGBUF_F_MPENQ | P64_RINGBUF_F_LFDEQ);
    printf("testing SPLFC ring buffer\n");
    test_rb(P64_RINGBUF_F_SPENQ | P64_RINGBUF_F_LFDEQ);
    printf("ringbuf test complete\n");
    return 0;
}
