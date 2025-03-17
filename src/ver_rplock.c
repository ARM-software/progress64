#include <stdbool.h>
#include <stdlib.h>

#include "p64_rplock.h"
#include "os_abstraction.h"
#include "verify.h"
#include "atomic.h"

#define NUMTHREADS 2

static p64_rplock_t rp_lock;
static bool rp_taken;

static void
ver_rplock_init(uint32_t numthreads)
{
    if (numthreads != NUMTHREADS)
    {
	abort();
    }
    p64_rplock_init(&rp_lock);
    rp_taken = false;
}

static void
ver_rplock_fini(uint32_t numthreads)
{
    (void)numthreads;
    VERIFY_ASSERT(rp_taken == false);
}

static void
ver_rplock_exec(uint32_t id)
{
    (void)id;
    p64_rpnode_t node;
    p64_rplock_acquire(&rp_lock, &node);
    VERIFY_ASSERT(regular_load_n(&rp_taken) == false);
    regular_store_n(&rp_taken, true);
    VERIFY_ASSERT(regular_load_n(&rp_taken) == true);
    regular_store_n(&rp_taken, false);
    p64_rplock_release(&rp_lock, &node);
}

struct ver_funcs ver_rplock =
{
    "rplock", ver_rplock_init, ver_rplock_exec, ver_rplock_fini
};
