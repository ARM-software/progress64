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

static inline intptr_t
ptr_to_int(void *ptr)
{
    assert(ptr != NULL);
    return (intptr_t)ptr - 1;
}


static inline void *
int_to_ptr(intptr_t sint)
{
    return (void *)(sint + 1);
}

static void
destructor(void *ptr)
{
    uint32_t idx = ptr_to_int(ptr);
    __atomic_fetch_and(&thread_words[idx / 64], ~(1U << (idx % 64)), __ATOMIC_RELEASE);
}

void
very_first_time(void)
{
    if (pthread_key_create(&key, destructor) != 0)
    {
	perror("pthread_key_create"), exit(EXIT_FAILURE);
    }
}

int32_t
p64_idx_alloc(void)
{
    pthread_once(&once, very_first_time);
    void *ptr = pthread_getspecific(key);
    if (ptr != NULL)
    {
	return ptr_to_int(ptr);
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
		int32_t idx = 64 * i + bit;
		if ((errno = pthread_setspecific(key, int_to_ptr(idx))) != 0)
		{
		    perror("pthread_setspecific"), exit(EXIT_FAILURE);
		}
		return idx;
	    }
	}
    }
    return -1;
}

void
p64_idx_free(int32_t idx)
{
    if (idx < 0 || idx >= MAXTHREADS ||
	(__atomic_load_n(&thread_words[idx / 64], __ATOMIC_RELAXED) & (1U << (idx % 64))) == 0)
    {
	fprintf(stderr, "Invalid thread index %d\n", idx), abort();
    }
    __atomic_fetch_and(&thread_words[idx / 64], ~(1U << (idx % 64)), __ATOMIC_RELEASE);
}
