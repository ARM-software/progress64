#include <stdbool.h>
#include <stdlib.h>

#include "p64_hemlock.h"
#include "os_abstraction.h"
#include "verify.h"
#include "atomic.h"

#define NUMTHREADS 2

static p64_hemlock_t hem_lock;
static bool hem_taken = false;

static void
ver_hemlock_init(uint32_t numthreads)
{
    if (numthreads != NUMTHREADS)
    {
	abort();
    }
    p64_hemlock_init(&hem_lock);
    hem_taken = false;
}

static void
ver_hemlock_fini(uint32_t numthreads)
{
    (void)numthreads;
    VERIFY_ASSERT(hem_taken == false);
}

static void
ver_hemlock_exec(uint32_t id)
{
    (void)id;
    p64_hemlock_acquire(&hem_lock);
    VERIFY_ASSERT(regular_load_n(&hem_taken) == false);
    regular_store_n(&hem_taken, true);
    VERIFY_YIELD();
    VERIFY_ASSERT(regular_load_n(&hem_taken) == true);
    regular_store_n(&hem_taken, false);
    p64_hemlock_release(&hem_lock);
}

struct ver_funcs ver_hemlock =
{
    "hemlock", ver_hemlock_init, ver_hemlock_exec, ver_hemlock_fini
};
