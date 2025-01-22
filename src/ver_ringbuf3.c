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

static void
ver_ringbuf3_init(uint32_t numthreads)
{
    if (numthreads != NUMTHREADS)
    {
	abort();
    }
    rb_rb = p64_ringbuf_alloc(64, P64_RINGBUF_F_SPENQ | P64_RINGBUF_F_SCDEQ, sizeof(void *));
    VERIFY_ASSERT(rb_rb != NULL);
    rb_elems[0] = 0;
    rb_elems[1] = 1;
}

static void
ver_ringbuf3_fini(uint32_t numthreads)
{
    (void)numthreads;
    p64_ringbuf_free(rb_rb);
}

static void
ver_ringbuf3_exec(uint32_t id)
{
    if (id == 0)
    {
	//Producer enqueues 2 elements
	uint32_t *elem;
	elem = &rb_elems[0];
	VERIFY_ASSERT(p64_ringbuf_enqueue(rb_rb, (void **)&elem, 1) == 1);
	elem = &rb_elems[1];
	VERIFY_ASSERT(p64_ringbuf_enqueue(rb_rb, (void **)&elem, 1) == 1);
	(void)elem;
    }
    else //id == 1
    {
	//Consumer dequeues 2 elements
	uint32_t idx;
	uint32_t *elem;
	elem = NULL;
	while (p64_ringbuf_dequeue(rb_rb, (void **)&elem, 1, &idx) == 0)
	{
	    VERIFY_YIELD();
	}
	VERIFY_ASSERT(idx == 0);
	VERIFY_ASSERT(elem == &rb_elems[0]);
	VERIFY_ASSERT(*elem == 0);
	elem = NULL;
	while (p64_ringbuf_dequeue(rb_rb, (void **)&elem, 1, &idx) == 0)
	{
	    VERIFY_YIELD();
	}
	VERIFY_ASSERT(idx == 1);
	VERIFY_ASSERT(elem == &rb_elems[1]);
	VERIFY_ASSERT(*elem == 1);
	(void)elem;
    }
}

struct ver_funcs ver_ringbuf3 =
{
    "ringbuf3", ver_ringbuf3_init, ver_ringbuf3_exec, ver_ringbuf3_fini
};
