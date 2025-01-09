//Copyright (c) 2025, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "p64_blkring.h"

#include "verify.h"

#define NUMTHREADS 2

static p64_blkring_t *blkr_rb;
static uint32_t *blkr_elems[NUMTHREADS];

static void
ver_blkring_init(uint32_t numthreads)
{
    if (numthreads != NUMTHREADS)
    {
	abort();
    }
    blkr_rb = p64_blkring_alloc(64);
    VERIFY_ASSERT(blkr_rb != NULL);
}

static void
ver_blkring_fini(uint32_t numthreads)
{
    (void)numthreads;
    //VERIFY_ASSERT(blkr_elems[0] == NULL && blkr_elems[1] == NULL);
    p64_blkring_free(blkr_rb);
}

static void
ver_blkring_exec(uint32_t id)
{
    uint32_t idx;
    uint32_t *elem = malloc(sizeof(uint32_t));
    VERIFY_ASSERT(elem != NULL);
    *elem = id;
    blkr_elems[id] = elem;
    p64_blkring_enqueue(blkr_rb, (void **)&elem, 1);
    p64_blkring_dequeue(blkr_rb, (void **)&elem, 1, &idx);
    VERIFY_ASSERT(elem == blkr_elems[0] || elem == blkr_elems[1]);
    if (elem == blkr_elems[0])
    {
	VERIFY_ASSERT(*elem == 0);
	blkr_elems[0] = NULL;
    }
    else
    {
	VERIFY_ASSERT(*elem == 1);
	blkr_elems[1] = NULL;
    }
    free(elem);
}

struct ver_funcs ver_blkring =
{
    "blkring", ver_blkring_init, ver_blkring_exec, ver_blkring_fini
};
