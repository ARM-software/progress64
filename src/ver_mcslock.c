//Copyright (c) 2025, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdbool.h>
#include <stdlib.h>

#include "p64_mcslock.h"
#include "os_abstraction.h"
#include "verify.h"

#define NUMTHREADS 2

static p64_mcslock_t mcs_lock;
static bool mcs_taken = false;

static void
ver_mcslock_init(uint32_t numthreads)
{
    if (numthreads != NUMTHREADS)
    {
	abort();
    }
    p64_mcslock_init(&mcs_lock);
    mcs_taken = false;
}

static void
ver_mcslock_fini(uint32_t numthreads)
{
    (void)numthreads;
    VERIFY_ASSERT(mcs_taken == false);
}

static void
ver_mcslock_exec(uint32_t id)
{
    (void)id;
    p64_mcsnode_t node;
    p64_mcslock_acquire(&mcs_lock, &node);
    VERIFY_ASSERT(mcs_taken == false);
    mcs_taken = true;
    VERIFY_SUSPEND(V_OP, "nop", NULL, 0, 0, 0);
    mcs_taken = false;
    p64_mcslock_release(&mcs_lock, &node);
}

struct ver_funcs ver_mcslock =
{
    "mcslock", ver_mcslock_init, ver_mcslock_exec, ver_mcslock_fini
};
