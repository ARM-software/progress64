//Copyright (c) 2025, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdbool.h>
#include <stdlib.h>

#include "p64_clhlock.h"
#include "os_abstraction.h"
#include "verify.h"
#include "atomic.h"

#define NUMTHREADS 2

static p64_clhlock_t clh_lock;
static bool clh_taken = false;

static void
ver_clhlock_init(uint32_t numthreads)
{
    if (numthreads != NUMTHREADS)
    {
	abort();
    }
    p64_clhlock_init(&clh_lock);
    clh_taken = false;

}

static void
ver_clhlock_fini(uint32_t numthreads)
{
    (void)numthreads;
    p64_clhlock_fini(&clh_lock);
    VERIFY_ASSERT(clh_taken == false);
}

static void
ver_clhlock_exec(uint32_t id)
{
    (void)id;
    p64_clhnode_t *node = NULL;
    p64_clhlock_acquire(&clh_lock, &node);
    VERIFY_ASSERT(regular_load_n(&clh_taken) == false);
    regular_store_n(&clh_taken, true);
    VERIFY_YIELD();
    VERIFY_ASSERT(regular_load_n(&clh_taken) == true);
    regular_store_n(&clh_taken, false);
    p64_clhlock_release(&node);
    p64_mfree(node);
}

struct ver_funcs ver_clhlock =
{
    "clhlock", ver_clhlock_init, ver_clhlock_exec, ver_clhlock_fini
};
