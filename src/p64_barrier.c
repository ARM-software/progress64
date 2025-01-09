//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdbool.h>

#include "p64_barrier.h"
#include "build_config.h"

#include "arch.h"
#include "err_hnd.h"
#include "atomic.h"

void
p64_barrier_init(p64_barrier_t *br, uint32_t numthreads)
{
    if (numthreads == 0 || numthreads > 0x10000000)
    {
	report_error("barrier", "invalid number of threads", numthreads);
	return;
    }
    br->numthr = numthreads;
    br->waiting = 0;
}

//Alternates between even (0) and odd (1) laps
#define LAP(cnt, nthr) (((cnt) / (nthr)) % 2)

//The barrier counts from 0 to 2N-1 and then wraps back to 0
//Intermittently values from 2N to 3N-1 may exist but these are equivalent to
//values 0 to N-1
void
p64_barrier_wait(p64_barrier_t *br)
{
    uint32_t before = atomic_fetch_add(&br->waiting, 1, __ATOMIC_ACQ_REL);
    if (before + 1 == 2 * br->numthr)
    {
	//Wrap back to 0
	//Barrier count may already have incremented again so perform
	//incremental wrap using subtraction
	atomic_fetch_sub(&br->waiting, 2 * br->numthr, __ATOMIC_RELAXED);
    }
    else
    {
	register uint32_t numthr = br->numthr;
	uint32_t cur_lap = LAP(before, numthr);
	while (LAP(atomic_ldx(&br->waiting, __ATOMIC_ACQUIRE), numthr) == cur_lap)
	{
	    WFE();
	}
    }
}
