//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _P64_ANTIREPLAY_H
#define _P64_ANTIREPLAY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef uint64_t p64_antireplay_sn_t;

typedef enum
{
    pass, replay, stale
} p64_antireplay_result_t;

struct p64_antireplay;
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
