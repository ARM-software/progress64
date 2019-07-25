//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifdef _WIN32
#include <processthreadsapi.h>
#define aligned_alloc(al, sz) _aligned_malloc((sz), (al))
#elif defined __APPLE__
#include <pthread.h>
#elif defined __linux__
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sys/syscall.h>
#include <unistd.h>
#endif
#include <stdlib.h>

#include "os_abstraction.h"
#include "common.h"

uint64_t
p64_gettid(void)
{
#if defined _WIN32
    return GetCurrentThreadId();
#elif defined __APPLE__
    uint64_t tid;
    pthread_threadid_np(NULL, &tid);
    return tid;
#elif defined __linux__
    return syscall(__NR_gettid);
#else
#error Unsupported OS
#endif
}

void *
p64_malloc(size_t size, size_t alignment)
{
    void *ptr;
    if (alignment > 1)
    {
#ifdef __APPLE__
	if (alignment < sizeof(void *))
	{
	    alignment = sizeof(void *);
	}
	if (posix_memalign(&ptr, alignment, size) != 0)
	{
	    //Failure
	    ptr = NULL;
	}
#else
	ptr = aligned_alloc(alignment, ROUNDUP(size, alignment));
#endif
    }
    else
    {
	ptr = malloc(size);
    }
    return ptr;
}

void
p64_mfree(void *ptr)
{
    free(ptr);
}
