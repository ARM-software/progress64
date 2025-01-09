//Copyright (c) 2025, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdlib.h>
#include "p64_msqueue.h"
#include "os_abstraction.h"
#include "verify.h"

#define NUMTHREADS 2

static p64_ptr_tag_t msq_head;
static p64_ptr_tag_t msq_tail;

static void
ver_msqueue_init(uint32_t numthreads)
{
    if (numthreads != NUMTHREADS)
    {
	abort();
    }
    p64_msqueue_elem_t *dummy = p64_malloc(sizeof(p64_msqueue_elem_t) + sizeof(uint32_t), 64);
    VERIFY_ASSERT(dummy != NULL);
    dummy->max_size = sizeof(uint32_t);
    p64_msqueue_init(&msq_head, &msq_tail, P64_ABA_TAG, dummy);
}

static void
ver_msqueue_fini(uint32_t numthreads)
{
    (void)numthreads;
    p64_msqueue_elem_t *dummy = p64_msqueue_fini(&msq_head, &msq_tail);
    //VERIFY_ASSERT(dummy != NULL);
    p64_mfree(dummy);
}

static void
ver_msqueue_exec(uint32_t id)
{
    p64_msqueue_elem_t *elem = p64_malloc(sizeof(p64_msqueue_elem_t) + sizeof(uint32_t), 64);
    VERIFY_ASSERT(elem != NULL);
    elem->max_size = sizeof(uint32_t);
    uint32_t data = id, sizeof_data = sizeof data;
    p64_msqueue_enqueue(&msq_head, &msq_tail, elem, &data, sizeof_data);
    elem = p64_msqueue_dequeue(&msq_head, &msq_tail, &data, &sizeof_data);
    VERIFY_ASSERT(elem != NULL && sizeof_data == sizeof data);
    VERIFY_ASSERT(data == 0 || data == 1);
}

struct ver_funcs ver_msqueue =
{
    "msqueue", ver_msqueue_init, ver_msqueue_exec, ver_msqueue_fini
};
