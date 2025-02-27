//Copyright (c) 2025, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdbool.h>
#include <stdlib.h>

#include "p64_spinlock.h"
#include "os_abstraction.h"
#include "verify.h"
#include "atomic.h"

#define NUMTHREADS 2

static p64_spinlock_t spin_lock;
static bool spin_taken = false;

static void
ver_spinlock_init(uint32_t numthreads)
{
    if (numthreads != NUMTHREADS)
    {
	abort();
    }
    p64_spinlock_init(&spin_lock);
    spin_taken = false;
}

static void
ver_spinlock_fini(uint32_t numthreads)
{
    (void)numthreads;
    VERIFY_ASSERT(spin_taken == false);
}

static void
ver_spinlock_exec(uint32_t id)
{
    (void)id;
    p64_spinlock_acquire(&spin_lock);
    VERIFY_ASSERT(regular_load_n(&spin_taken) == false);
    regular_store_n(&spin_taken, true);
    VERIFY_YIELD();
    VERIFY_ASSERT(regular_load_n(&spin_taken) == true);
    regular_store_n(&spin_taken, false);
    p64_spinlock_release(&spin_lock);
}

struct ver_funcs ver_spinlock =
{
    "spinlock", ver_spinlock_init, ver_spinlock_exec, ver_spinlock_fini
};
