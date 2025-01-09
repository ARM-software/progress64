//Copyright (c) 2025, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "p64_buckring.h"

#include "verify.h"

#define NUMTHREADS 2

static p64_buckring_t *buckr_rb;
static uint32_t *buckr_elems[NUMTHREADS];

static void
ver_buckring1_init(uint32_t numthreads)
{
    if (numthreads != NUMTHREADS)
    {
	abort();
    }
    buckr_rb = p64_buckring_alloc(64, 0);
    VERIFY_ASSERT(buckr_rb != NULL);
    uint32_t *elem0 = malloc(sizeof(uint32_t));
    uint32_t *elem1 = malloc(sizeof(uint32_t));
    VERIFY_ASSERT(elem0 != NULL);
    VERIFY_ASSERT(elem1 != NULL);
    *elem0 = 0;
    *elem1 = 1;
    buckr_elems[0] = elem0;
    buckr_elems[1] = elem1;
}

static void
ver_buckring1_fini(uint32_t numthreads)
{
    (void)numthreads;
    VERIFY_ASSERT(buckr_elems[0] == NULL && buckr_elems[1] == NULL);
    p64_buckring_free(buckr_rb);
}

static void
ver_buckring1_exec(uint32_t id)
{
    (void)id;
    uint32_t idx;
    VERIFY_ASSERT(p64_buckring_enqueue(buckr_rb, (void **)&buckr_elems[id], 1) == 1);
    //We cannot successfully dequeue element until all preceding enqueue's have completed
    uint32_t *elem = NULL;
    while (p64_buckring_dequeue(buckr_rb, (void **)&elem, 1, &idx) == 0)
    {
	VERIFY_YIELD();
    }
    VERIFY_ASSERT(idx == 0 || idx == 1);
    VERIFY_ASSERT(elem == buckr_elems[0] || elem == buckr_elems[1]);
    if (elem == buckr_elems[0])
    {
	VERIFY_ASSERT(*elem == 0);
	buckr_elems[0] = NULL;
    }
    else
    {
	VERIFY_ASSERT(*elem == 1);
	buckr_elems[1] = NULL;
    }
    free(elem);
}

struct ver_funcs ver_buckring1 =
{
    "buckring1", ver_buckring1_init, ver_buckring1_exec, ver_buckring1_fini
};
