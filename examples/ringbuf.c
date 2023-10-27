//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <inttypes.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "p64_errhnd.h"
#include "p64_ringbuf_template.h"

//Instantiate the ring buffer template using uintptr_t as the ring element
P64_RINGBUF(p64_ringbuf_uip, uintptr_t)

#include "expect.h"

static jmp_buf jmpbuf;

static int
error_handler(const char *module, const char *error, uintptr_t val)
{
    EXPECT(strcmp(module, "ringbuf") == 0 &&
	   strcmp(error, "invalid flags") == 0 &&
	   val == (P64_RINGBUF_F_NBDEQ | P64_RINGBUF_F_LFDEQ));
    longjmp(jmpbuf, 1);
    //Not reached
}

static void
test_rb(uint32_t flags)
{
    uintptr_t vec[6];
    int ret;
    uint32_t index;

    p64_ringbuf_uip_t *rb = p64_ringbuf_uip_alloc(5, flags);
    if (flags == (P64_RINGBUF_F_NBDEQ | P64_RINGBUF_F_LFDEQ))
    {
	EXPECT(rb == NULL);
	return;
    }
    EXPECT(rb != NULL);

    ret = p64_ringbuf_uip_dequeue(rb, vec, 1, &index);
    EXPECT(ret == 0);
    ret = p64_ringbuf_uip_enqueue(rb, (uintptr_t[]){ 1 }, 1);
    EXPECT(ret == 1);

    ret = p64_ringbuf_uip_dequeue(rb, vec, 1, &index);
    EXPECT(ret == 1);
    EXPECT(index == 0);
    EXPECT(vec[0] == 1);

    ret = p64_ringbuf_uip_dequeue(rb, vec, 1, &index);
    EXPECT(ret == 0);

    ret = p64_ringbuf_uip_enqueue(rb, (uintptr_t[]){ 2, 3, 4, 5, 6, 7 }, 6);
    EXPECT(ret == 5);

    ret = p64_ringbuf_uip_dequeue(rb, vec, 1, &index);
    EXPECT(ret == 1);
    EXPECT(index == 1);
    EXPECT(vec[0] == 2);

    ret = p64_ringbuf_uip_dequeue(rb, vec, 6, &index);
    EXPECT(ret == 4);
    EXPECT(index == 2);
    EXPECT(vec[0] == 3);
    EXPECT(vec[1] == 4);
    EXPECT(vec[2] == 5);
    EXPECT(vec[3] == 6);

    p64_ringbuf_uip_free(rb);
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
    printf("testing NBENQ/LFDEQ ring buffer\n");
    test_rb(P64_RINGBUF_F_NBENQ | P64_RINGBUF_F_LFDEQ);
    printf("testing NBDEQ/LFDEQ ring buffer (invalid)\n");//Invalid flags
    p64_errhnd_install(error_handler);
    if (setjmp(jmpbuf) == 0)
    {
	test_rb(P64_RINGBUF_F_NBDEQ | P64_RINGBUF_F_LFDEQ);
    }
    //Else longjumped back from error handler
    printf("ringbuf test complete\n");
    return 0;
}
