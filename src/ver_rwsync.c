//Copyright (c) 2025, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdbool.h>
#include <stdlib.h>

#include "p64_rwsync.h"
#include "os_abstraction.h"
#include "verify.h"
#include "atomic.h"

#define NUMTHREADS 2

static p64_rwsync_t rws;
static bool taken = false;

static void
ver_rwsync_init(uint32_t numthreads)
{
    if (numthreads != NUMTHREADS)
    {
	abort();
    }
    p64_rwsync_init(&rws);
    taken = false;
}

static void
ver_rwsync_fini(uint32_t numthreads)
{
    (void)numthreads;
    VERIFY_ASSERT(taken == false);
}

static void
ver_rwsync_exec(uint32_t id)
{
    (void)id;
    p64_rwsync_acquire_wr(&rws);
    VERIFY_ASSERT(regular_load_n(&taken) == false);
    regular_store_n(&taken, true);
    VERIFY_ASSERT(regular_load_n(&taken) == true);
    regular_store_n(&taken, false);
    p64_rwsync_release_wr(&rws);

    p64_rwsync_t prv;
    bool t;
    (void)t;
    do
    {
	prv = p64_rwsync_acquire_rd(&rws);
	//Must use atomic load inside reader-side "critical section"
	t = atomic_load_n(&taken, __ATOMIC_RELAXED);
	//Delay check of 't' until after we know we have a proper reading
    }
    while (!p64_rwsync_release_rd(&rws, prv));
    VERIFY_ASSERT(t == false);
}

struct ver_funcs ver_rwsync =
{
    "rwsync", ver_rwsync_init, ver_rwsync_exec, ver_rwsync_fini
};
