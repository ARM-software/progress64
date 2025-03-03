//Copyright (c) 2025, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdint.h>
#include "p64_mcqueue.h"
#include "verify.h"
#include "atomic.h"

#define NUMTHREADS 2

struct elem
{
    p64_mcqueue_elem_t node;
    uint32_t id;
};

static p64_mcqueue_t mcq_queue;
static struct elem mcq_elems[NUMTHREADS];
static uint32_t mcq_mask;

static void
ver_mcqueue_init(uint32_t numthreads)
{
    if (numthreads != NUMTHREADS)
    {
	abort();
    }
    mcq_mask = 0;
    p64_mcqueue_init(&mcq_queue);
}

static void
ver_mcqueue_fini(uint32_t numthreads)
{
    (void)numthreads;
    //Check that both elements were dequeued
    VERIFY_ASSERT(mcq_mask == 3);
}

static void
ver_mcqueue_exec(uint32_t id)
{
    struct elem *elem = &mcq_elems[id];
    regular_store_n(&elem->id, id);
    p64_mcqueue_enqueue(&mcq_queue, &elem->node);
    elem = (struct elem *)p64_mcqueue_dequeue(&mcq_queue);
    VERIFY_ASSERT(elem != NULL);
    //Update mask with id of element that was dequeued
    mcq_mask |= 1U << regular_load_n(&elem->id);
}

struct ver_funcs ver_mcqueue =
{
    "mcqueue", ver_mcqueue_init, ver_mcqueue_exec, ver_mcqueue_fini
};
