//Copyright (c) 2025, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "p64_ringbuf.h"

#include "verify.h"

#define NUMTHREADS 2

static p64_ringbuf_t *rb_rb;
static uint32_t rb_elems[NUMTHREADS];
static uint32_t rb_mask;

static void
ver_ringbuf1_init(uint32_t numthreads)
{
    if (numthreads != NUMTHREADS)
    {
	abort();
    }
    rb_rb = p64_ringbuf_alloc(64, P64_RINGBUF_F_NBENQ | P64_RINGBUF_F_NBDEQ, sizeof(void *));
    VERIFY_ASSERT(rb_rb != NULL);
    rb_elems[0] = 0;
    rb_elems[1] = 1;
    rb_mask = 0;
}

static void
ver_ringbuf1_fini(uint32_t numthreads)
{
    (void)numthreads;
    VERIFY_ASSERT(rb_mask == 0x33);
    p64_ringbuf_free(rb_rb);
}

static void
ver_ringbuf1_exec(uint32_t id)
{
    (void)id;
    uint32_t idx;
    uint32_t *elem = &rb_elems[id];
    VERIFY_ASSERT(p64_ringbuf_enqueue(rb_rb, (void **)&elem, 1) == 1);
    rb_mask ^= 1U << id;
    //We cannot successfully dequeue element until all preceding enqueues have completed
    elem = NULL;
    while (p64_ringbuf_dequeue(rb_rb, (void **)&elem, 1, &idx) == 0)
    {
	VERIFY_YIELD();
    }
    VERIFY_ASSERT(idx == 0 || idx == 1);
    VERIFY_ASSERT(elem == &rb_elems[0] || elem == &rb_elems[1]);
    if (elem == &rb_elems[0])
    {
	VERIFY_ASSERT(*elem == 0);
    }
    else
    {
	VERIFY_ASSERT(*elem == 1);
    }
    rb_mask ^= 16U << *elem;
}

struct ver_funcs ver_ringbuf1 =
{
    "ringbuf1", ver_ringbuf1_init, ver_ringbuf1_exec, ver_ringbuf1_fini
};
