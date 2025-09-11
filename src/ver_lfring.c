//Copyright (c) 2025, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "p64_lfring.h"
#include "verify.h"
#include "atomic.h"

#define NUMTHREADS 2

static p64_lfring_t *lfr_rb;
static uint32_t lfr_elems[NUMTHREADS];
static uint32_t lfr_mask;

static void
ver_lfring_init(uint32_t numthreads)
{
    if (numthreads != NUMTHREADS)
    {
	abort();
    }
    lfr_rb = p64_lfring_alloc(64, P64_LFRING_F_MPENQ | P64_LFRING_F_MCDEQ);
    VERIFY_ASSERT(lfr_rb != NULL);
    lfr_mask = 0;
}

static void
ver_lfring_fini(uint32_t numthreads)
{
    (void)numthreads;
    VERIFY_ASSERT(lfr_mask == 0x33);
    p64_lfring_free(lfr_rb);
}

static void
ver_lfring_exec(uint32_t id)
{
    (void)id;
    uint32_t idx;
    (void)idx;
    uint32_t *elem = &lfr_elems[id];
    *elem = id;
    VERIFY_ASSERT(p64_lfring_enqueue(lfr_rb, (void **)&elem, 1) == 1);
    lfr_mask ^= 1U << id;
    elem = NULL;
    VERIFY_ASSERT(p64_lfring_dequeue(lfr_rb, (void **)&elem, 1, &idx) == 1);
    VERIFY_ASSERT(idx == 0 || idx == 1);
    VERIFY_ASSERT(elem == &lfr_elems[0] || elem == &lfr_elems[1]);
    if (elem == &lfr_elems[0])
    {
	VERIFY_ASSERT(*elem == 0);
    }
    else
    {
	VERIFY_ASSERT(*elem == 1);
    }
    lfr_mask ^= 16U << *elem;
}

struct ver_funcs ver_lfring =
{
    "lfring", ver_lfring_init, ver_lfring_exec, ver_lfring_fini
};
