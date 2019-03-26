//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include "common.h"
#include "thr_idx.h"
#include "build_config.h"

#define NWORDS ((MAXTHREADS + 63) / 64)

static uint64_t thread_words[NWORDS];

static pthread_once_t once = PTHREAD_ONCE_INIT;
static pthread_key_t key;

struct idx_cnt
{
    uint16_t idx;
    uint16_t cnt;
};

static inline struct idx_cnt
uint_to_idxcnt(uintptr_t uint)
{
    struct idx_cnt ic;
    ic.idx = (uint16_t)(uint & 0xffff);
    ic.cnt = (uint16_t)(uint >> 16);
    return ic;
}

static inline uintptr_t
idxcnt_to_uint(struct idx_cnt ic)
{
    return ic.idx | (ic.cnt << 16);
}

static inline struct idx_cnt
ptr_to_idxcnt(void *ptr)
{
    assert(ptr != NULL);
    return uint_to_idxcnt((uintptr_t)ptr - 1);
}


static inline void *
idxcnt_to_ptr(struct idx_cnt ic)
{
    return (void *)(idxcnt_to_uint(ic) + 1);
}

static void
destructor(void *ptr)
{
    uint32_t idx = ptr_to_idxcnt(ptr).idx;
    __atomic_fetch_and(&thread_words[idx / 64],
		       ~(1U << (idx % 64)),
		       __ATOMIC_RELEASE);
}

void
very_first_time(void)
{
    if ((errno = pthread_key_create(&key, destructor)) != 0)
    {
	perror("pthread_key_create"), exit(EXIT_FAILURE);
    }
}

static void
set_key(struct idx_cnt ic)
{
    assert(ic.cnt != 0);
    if ((errno = pthread_setspecific(key, idxcnt_to_ptr(ic))) != 0)
    {
	perror("pthread_setspecific"), exit(EXIT_FAILURE);
    }
}

int32_t
p64_idx_alloc(void)
{
    pthread_once(&once, very_first_time);
    void *ptr = pthread_getspecific(key);
    if (ptr != NULL)
    {
	struct idx_cnt ic = ptr_to_idxcnt(ptr);
	ic.cnt++;
	set_key(ic);
	return ic.idx;
    }

    for (uint32_t i = 0; i < NWORDS; i++)
    {
	uint64_t word = thread_words[i];
	while (~word != 0)
	{
	    uint32_t bit = __builtin_ctz(~word);
	    if (64 * i + bit >= MAXTHREADS)
	    {
		return -1;
	    }
	    if (__atomic_compare_exchange_n(&thread_words[i],
					    &word,
					    word | (1U << bit),
					    /*weak*/0,
					    __ATOMIC_ACQUIRE,
					    __ATOMIC_RELAXED))
	    {
		struct idx_cnt ic;
		ic.idx = 64 * i + bit;
		ic.cnt = 1;
		set_key(ic);
		return ic.idx;
	    }
	}
    }
    return -1;
}

void
p64_idx_free(int32_t idx)
{
    void *ptr = pthread_getspecific(key);
    if (ptr != NULL)
    {
	struct idx_cnt ic = ptr_to_idxcnt(ptr);
	if (ic.idx == idx)
	{
	    assert (ic.cnt != 0);
	    assert((__atomic_load_n(&thread_words[idx / 64], __ATOMIC_RELAXED) &
				    (1U << (idx % 64))) != 0);
	    if (--ic.cnt == 0)
	    {
		//Relinquish this thread index
		__atomic_fetch_and(&thread_words[idx / 64],
				   ~(1U << (idx % 64)), __ATOMIC_RELEASE);
		pthread_setspecific(key, NULL);
	    }
	    else
	    {
		set_key(ic);
	    }
	    return;
	}
	//Else mismatching thread index
    }
    //Else pthread TLS not set
    fprintf(stderr, "Mismatched call to p64_idx_free(idx=%d)\n", idx);
    fflush(stderr);
    abort();
}
