//Copyright (c) 2025, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdbool.h>
#include <stdlib.h>

#include "p64_barrier.h"
#include "os_abstraction.h"
#include "verify.h"

#define NUMTHREADS 2

static p64_barrier_t barrier;
static uint32_t count;

static void
ver_barrier_init(uint32_t numthreads)
{
    if (numthreads != NUMTHREADS)
    {
	abort();
    }
    p64_barrier_init(&barrier, 2);
    count = 0;
}

static void
ver_barrier_fini(uint32_t numthreads)
{
    (void)numthreads;
}

static void
ver_barrier_exec(uint32_t id)
{
    (void)id;
    p64_barrier_wait(&barrier);
    count++;
    VERIFY_ASSERT(count == 1 || count == 2);
    p64_barrier_wait(&barrier);
    count++;
    VERIFY_ASSERT(count == 3 || count == 4);
    p64_barrier_wait(&barrier);
    VERIFY_ASSERT(count == 4);
}

struct ver_funcs ver_barrier =
{
    "barrier", ver_barrier_init, ver_barrier_exec, ver_barrier_fini
};
