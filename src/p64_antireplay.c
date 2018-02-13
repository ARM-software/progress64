//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "p64_antireplay.h"
#include "build_config.h"

#include "lockfree.h"
#include "common.h"

struct p64_antireplay
{
    uint32_t winmask;
    bool swizzle;
    p64_antireplay_sn_t snv[] ALIGNED(CACHE_LINE);
};

p64_antireplay_t *
p64_antireplay_alloc(uint32_t winsize, bool swizzle)
{
    if (winsize == 0 || !IS_POWER_OF_TWO(winsize))
    {
	fprintf(stderr, "Invalid window size %u\n", winsize), abort();
    }
    size_t nbytes = ROUNDUP(sizeof(p64_antireplay_t) +
			    winsize * sizeof(p64_antireplay_sn_t),
			    CACHE_LINE);
    p64_antireplay_t *arwin = aligned_alloc(CACHE_LINE, nbytes);
    if (arwin != NULL)
    {
	//Clear all sequence numbers
	memset(arwin, 0, nbytes);
	arwin->winmask = winsize - 1;
	arwin->swizzle = swizzle;
	return arwin;
    }
    return NULL;
}

void
p64_antireplay_free(p64_antireplay_t *arwin)
{
    free(arwin);
}

static inline uint32_t
extrat_bits(uint32_t v, uint32_t msb, uint32_t lsb)
{
    uint32_t width = msb - lsb + 1;
    uint32_t mask = (1U << width) - 1U;
    return v & (mask << lsb);
}

static inline uint32_t
sn_to_index(p64_antireplay_t *arwin, p64_antireplay_sn_t sn)
{
    if (arwin->swizzle)
    {
	//Compute index to sequence number array but consecutive sequence
	//numbers will be located in different cache lines
	return (extrat_bits(sn, 31, 6) |
		extrat_bits(sn, 5, 3) >> 3 |
		extrat_bits(sn, 2, 0) << 3) & arwin->winmask;
    }
    else
    {
	return sn & arwin->winmask;
    }
}

p64_antireplay_result_t
p64_antireplay_test(p64_antireplay_t *arwin,
		    p64_antireplay_sn_t sn)
{
    uint32_t index = sn_to_index(arwin, sn);
    p64_antireplay_sn_t old = __atomic_load_n(&arwin->snv[index],
					      __ATOMIC_RELAXED);
    if (sn > old)
    {
	return pass;
    }
    else if (sn == old)
    {
	return replay;
    }
    else
    {
	return stale;
    }
}

p64_antireplay_result_t
p64_antireplay_test_and_set(p64_antireplay_t *arwin,
			    p64_antireplay_sn_t sn)
{
    uint32_t index = sn_to_index(arwin, sn);
    p64_antireplay_sn_t old = lockfree_fetch_umax_8(&arwin->snv[index],
						    sn,
						    __ATOMIC_RELAXED,
						    __ATOMIC_RELAXED);
    if (sn > old)
    {
	return pass;
    }
    else if (sn == old)
    {
	return replay;
    }
    else
    {
	return stale;
    }
}
