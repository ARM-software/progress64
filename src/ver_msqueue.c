//Copyright (c) 2025, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdlib.h>
#include "p64_msqueue.h"
#include "os_abstraction.h"
#include "verify.h"

#define NUMTHREADS 2

struct elem
{
    p64_msqueue_elem_t node;
    uint32_t data;
};

static p64_ptr_tag_t msq_head;
static p64_ptr_tag_t msq_tail;
static struct elem dummy;
static struct elem msq_elems[NUMTHREADS];

static void
ver_msqueue_init(uint32_t numthreads)
{
    if (numthreads != NUMTHREADS)
    {
	abort();
    }
    dummy.node.next.ptr = NULL;
    dummy.node.next.tag = ~0ul;//msqueue assertion
    dummy.node.max_size = sizeof(uint32_t);
    dummy.node.cur_size = 0;
    p64_msqueue_init(&msq_head, &msq_tail, P64_ABA_TAG, &dummy.node);
}

static void
ver_msqueue_fini(uint32_t numthreads)
{
    (void)numthreads;
    struct elem *dummy = (struct elem *)p64_msqueue_fini(&msq_head, &msq_tail);
    VERIFY_ASSERT(dummy == &msq_elems[0] || dummy == &msq_elems[1]);
}

static void
ver_msqueue_exec(uint32_t id)
{
    struct elem *elem = &msq_elems[id];
    elem->node.next.ptr = NULL;
    elem->node.next.tag = ~0ul;//msqueue assertion
    elem->node.max_size = sizeof(uint32_t);
    elem->node.cur_size = 0;
    uint32_t data = 242 + id, sizeof_data = sizeof data;
    p64_msqueue_enqueue(&msq_head, &msq_tail, &elem->node, &data, sizeof_data);
    elem = (struct elem *)p64_msqueue_dequeue(&msq_head, &msq_tail, &data, &sizeof_data);
    VERIFY_ASSERT(elem != NULL && sizeof_data == sizeof data);
    VERIFY_ASSERT(data == 242 || data == 243);
}

struct ver_funcs ver_msqueue =
{
    "msqueue", ver_msqueue_init, ver_msqueue_exec, ver_msqueue_fini
};
