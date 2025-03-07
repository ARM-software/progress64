//Copyright (c) 2025, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdint.h>
#include "p64_deque.h"
#include "verify.h"
#include "atomic.h"

#define NUMTHREADS 2

struct elem
{
    p64_deque_elem_t node;
    uint32_t id;
};

static p64_deque_t deq;
static struct elem deq_elems[NUMTHREADS + 1];
static uint32_t deq_mask;

static void
ver_deque1_init(uint32_t numthreads)
{
    if (numthreads != NUMTHREADS)
    {
	abort();
    }
    deq_mask = 0;
    p64_deque_init(&deq);
}

static void
ver_deque1_fini(uint32_t numthreads)
{
    (void)numthreads;
    //Check that both elements were dequeued
    VERIFY_ASSERT(deq_mask == 3);
}

static void
ver_deque1_exec(uint32_t id)
{
    struct elem *elem = &deq_elems[id];
    regular_store_n(&elem->id, id);
    p64_deque_enqueue_r(&deq, &elem->node);
    //Now dequeue from the same end
    elem = (struct elem *)p64_deque_dequeue_r(&deq);
    VERIFY_ASSERT(elem != NULL);
    //Update mask with id of element that was dequeued
    deq_mask |= 1U << regular_load_n(&elem->id);
}

struct ver_funcs ver_deque1 =
{
    "deque1", ver_deque1_init, ver_deque1_exec, ver_deque1_fini
};

static void
ver_deque2_init(uint32_t numthreads)
{
    if (numthreads != NUMTHREADS)
    {
	abort();
    }
    deq_mask = 0;
    p64_deque_init(&deq);
}

static void
ver_deque2_fini(uint32_t numthreads)
{
    (void)numthreads;
    //Check that both elements were dequeued
    VERIFY_ASSERT(deq_mask == 3);
}

static void
ver_deque2_exec(uint32_t id)
{
    struct elem *elem = &deq_elems[id];
    regular_store_n(&elem->id, id);
    p64_deque_enqueue_r(&deq, &elem->node);
    //Now dequeue from the other end
    elem = (struct elem *)p64_deque_dequeue_l(&deq);
    VERIFY_ASSERT(elem != NULL);
    //Update mask with id of element that was dequeued
    deq_mask |= 1U << regular_load_n(&elem->id);
}

struct ver_funcs ver_deque2 =
{
    "deque2", ver_deque2_init, ver_deque2_exec, ver_deque2_fini
};

static void
ver_deque3_init(uint32_t numthreads)
{
    if (numthreads != NUMTHREADS)
    {
	abort();
    }
    deq_mask = 0;
    p64_deque_init(&deq);
}

static void
ver_deque3_fini(uint32_t numthreads)
{
    (void)numthreads;
    //Check that two elements (out of three) were dequeued
    VERIFY_ASSERT(__builtin_popcount(deq_mask) == 2);
    VERIFY_ASSERT(deq.end[0] != NULL && deq.end[0] == deq.end[1]);
}

static void
ver_deque3_exec(uint32_t id)
{
    struct elem *elem = &deq_elems[id];
    regular_store_n(&elem->id, id);
    p64_deque_enqueue_r(&deq, &elem->node);
    if (id == 0)
    {
	//Only for the first thread
	elem = &deq_elems[NUMTHREADS];
	regular_store_n(&elem->id, NUMTHREADS);
	p64_deque_enqueue_l(&deq, &elem->node);
    }
    //Now dequeue from the other end
    elem = (struct elem *)p64_deque_dequeue_l(&deq);
    VERIFY_ASSERT(elem != NULL);
    //Update mask with id of element that was dequeued
    deq_mask |= 1U << regular_load_n(&elem->id);
}

struct ver_funcs ver_deque3 =
{
    "deque3", ver_deque3_init, ver_deque3_exec, ver_deque3_fini
};
