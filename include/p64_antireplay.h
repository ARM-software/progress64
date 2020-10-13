//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

//Lock-free/wait-free anti-replay window for replay protection
//Replay protection detects duplicate ("replayed") and stale events
//Sequence numbers are 64-bit

#ifndef P64_ANTIREPLAY_H
#define P64_ANTIREPLAY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef uint64_t p64_antireplay_sn_t;

typedef enum
{
    p64_ar_pass, p64_ar_replay, p64_ar_stale
} p64_antireplay_result_t;

typedef struct p64_antireplay p64_antireplay_t;

p64_antireplay_t *
p64_antireplay_alloc(uint32_t winsize,
		     bool swizzle);

void
p64_antireplay_free(p64_antireplay_t *arwin);

p64_antireplay_result_t
p64_antireplay_test(p64_antireplay_t *arwin,
		    p64_antireplay_sn_t sn);

p64_antireplay_result_t
p64_antireplay_test_and_set(p64_antireplay_t *arwin,
			    p64_antireplay_sn_t sn);

#ifdef __cplusplus
}
#endif

#endif
