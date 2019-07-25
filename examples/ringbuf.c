//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>

#include "p64_ringbuf_template.h"
//Instantiate the ring buffer template using uint64_t as the ring element
P64_RINGBUF(p64_ringbuf_ui64, uint64_t)

#include "expect.h"

static void
test_rb(uint32_t flags)
{
    uint64_t vec[4];
    int ret;
    uint32_t index;

    p64_ringbuf_ui64_t *rb = p64_ringbuf_ui64_alloc(2, flags);
    if (flags == (P64_RINGBUF_F_NBENQ | P64_RINGBUF_F_LFDEQ))
    {
	EXPECT(rb == NULL);
	return;
    }
    EXPECT(rb != NULL);

    ret = p64_ringbuf_ui64_dequeue(rb, vec, 1, &index);
    EXPECT(ret == 0);
    ret = p64_ringbuf_ui64_enqueue(rb, (uint64_t[]){ 1 }, 1);
    EXPECT(ret == 1);

    ret = p64_ringbuf_ui64_dequeue(rb, vec, 1, &index);
    EXPECT(ret == 1);
    EXPECT(index == 0);
    EXPECT(vec[0] == 1);

    ret = p64_ringbuf_ui64_dequeue(rb, vec, 1, &index);
    EXPECT(ret == 0);

    ret = p64_ringbuf_ui64_enqueue(rb, (uint64_t[]){ 2, 3, 4 }, 3);
    EXPECT(ret == 2);

    ret = p64_ringbuf_ui64_dequeue(rb, vec, 1, &index);
    EXPECT(ret == 1);
    EXPECT(index == 1);
    EXPECT(vec[0] == 2);

    ret = p64_ringbuf_ui64_dequeue(rb, vec, 4, &index);
    EXPECT(ret == 1);
    EXPECT(index == 2);
    EXPECT(vec[0] == 3);

    p64_ringbuf_ui64_free(rb);
}

int main(void)
{
    printf("testing MP/MC ring buffer\n");
    test_rb(P64_RINGBUF_F_MPENQ | P64_RINGBUF_F_MCDEQ);
    printf("testing SP/SC ring buffer\n");
    test_rb(P64_RINGBUF_F_SPENQ | P64_RINGBUF_F_SCDEQ);
    printf("testing MP/SC ring buffer\n");
    test_rb(P64_RINGBUF_F_MPENQ | P64_RINGBUF_F_SCDEQ);
    printf("testing SP/MC ring buffer\n");
    test_rb(P64_RINGBUF_F_SPENQ | P64_RINGBUF_F_MCDEQ);
    printf("testing MP/LFC ring buffer\n");
    test_rb(P64_RINGBUF_F_MPENQ | P64_RINGBUF_F_LFDEQ);
    printf("testing SP/LFC ring buffer\n");
    test_rb(P64_RINGBUF_F_SPENQ | P64_RINGBUF_F_LFDEQ);
    printf("testing NBMP/NBMC ring buffer\n");
    test_rb(P64_RINGBUF_F_NBENQ | P64_RINGBUF_F_NBDEQ);
    printf("testing NBMP/SC ring buffer\n");
    test_rb(P64_RINGBUF_F_NBENQ | P64_RINGBUF_F_SCDEQ);
    printf("testing NBMP/MC ring buffer\n");
    test_rb(P64_RINGBUF_F_NBENQ | P64_RINGBUF_F_MCDEQ);
    printf("testing MP/NBDEQ ring buffer\n");
    test_rb(P64_RINGBUF_F_MPENQ | P64_RINGBUF_F_NBDEQ);
    printf("testing SP/NBDEQ ring buffer\n");
    test_rb(P64_RINGBUF_F_SPENQ | P64_RINGBUF_F_NBDEQ);
    printf("testing NBEBQ/LFDEQ ring buffer\n");
    test_rb(P64_RINGBUF_F_NBENQ | P64_RINGBUF_F_LFDEQ);
    printf("ringbuf test complete\n");
    return 0;
}
