//Copyright (c) 2025, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "p64_buckring.h"

#include "verify.h"
#include "atomic.h"

#define NUMTHREADS 2

static p64_buckring_t *buckr_rb;
static uint32_t buckr_elems[NUMTHREADS];
static uint32_t buckr_mask;

static void
ver_buckring1_init(uint32_t numthreads)
{
    if (numthreads != NUMTHREADS)
    {
	abort();
    }
    buckr_rb = p64_buckring_alloc(64, 0);
    VERIFY_ASSERT(buckr_rb != NULL);
    buckr_elems[0] = 0;
    buckr_elems[1] = 1;
    buckr_mask = 0;
}

static void
ver_buckring1_fini(uint32_t numthreads)
{
    (void)numthreads;
    VERIFY_ASSERT(buckr_mask == 0x33);
    p64_buckring_free(buckr_rb);
}

static void
ver_buckring1_exec(uint32_t id)
{
    (void)id;
    uint32_t idx;
    uint32_t *elem = &buckr_elems[id];
    regular_store_n(elem, id);
    VERIFY_ASSERT(p64_buckring_enqueue(buckr_rb, (void **)&elem, 1) == 1);
    buckr_mask ^= 1U << id;
    //We cannot successfully dequeue element until all preceding enqueue's have completed
    elem = NULL;
    while (p64_buckring_dequeue(buckr_rb, (void **)&elem, 1, &idx) == 0)
    {
	VERIFY_YIELD();
    }
    VERIFY_ASSERT(idx == 0 || idx == 1);
    VERIFY_ASSERT(elem == &buckr_elems[0] || elem == &buckr_elems[1]);
    if (elem == &buckr_elems[0])
    {
	VERIFY_ASSERT(regular_load_n(elem) == 0);
    }
    else
    {
	VERIFY_ASSERT(regular_load_n(elem) == 1);
    }
    buckr_mask ^= 16U << *elem;
}

struct ver_funcs ver_buckring1 =
{
    "buckring1", ver_buckring1_init, ver_buckring1_exec, ver_buckring1_fini
};
