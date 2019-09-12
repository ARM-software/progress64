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
#include "os_abstraction.h"

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
    size_t nbytes = sizeof(p64_antireplay_t) +
		    winsize * sizeof(p64_antireplay_sn_t);
    p64_antireplay_t *ar = p64_malloc(nbytes, CACHE_LINE);
    if (ar != NULL)
    {
	//Clear all sequence numbers
	memset(ar, 0, nbytes);
	ar->winmask = winsize - 1;
	ar->swizzle = swizzle;
	return ar;
    }
    return NULL;
}

void
p64_antireplay_free(p64_antireplay_t *ar)
{
    p64_mfree(ar);
}

static inline uint32_t
sn_to_index(p64_antireplay_t *ar, p64_antireplay_sn_t sn)
{
    if (ar->swizzle)
    {
	//Compute index to sequence number array but consecutive sequence
	//numbers will be located in different cache lines
#if CACHE_LINE == 64
	//64B cache line and 8B (64-bit) array elements =>
	//8 elements/cache line
	sn = sn ^ (sn & 7) << 3;
#else
	//Assume 128B cache line => 16 elements/cache line
	sn = sn ^ (sn & 15) << 4;
#endif
    }
    return sn & ar->winmask;
}

p64_antireplay_result_t
p64_antireplay_test(p64_antireplay_t *ar,
		    p64_antireplay_sn_t sn)
{
    uint32_t index = sn_to_index(ar, sn);
    p64_antireplay_sn_t old = __atomic_load_n(&ar->snv[index],
					      __ATOMIC_RELAXED);
    if (sn > old)
    {
	return p64_ar_pass;
    }
    else if (sn == old)
    {
	return p64_ar_replay;
    }
    else
    {
	return p64_ar_stale;
    }
}

p64_antireplay_result_t
p64_antireplay_test_and_set(p64_antireplay_t *ar,
			    p64_antireplay_sn_t sn)
{
    uint32_t index = sn_to_index(ar, sn);
    p64_antireplay_sn_t old = lockfree_fetch_umax_8(&ar->snv[index],
						    sn, __ATOMIC_RELAXED);
    if (sn > old)
    {
	return p64_ar_pass;
    }
    else if (sn == old)
    {
	return p64_ar_replay;
    }
    else
    {
	return p64_ar_stale;
    }
}
