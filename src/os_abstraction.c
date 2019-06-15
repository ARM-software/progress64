//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifdef _WIN32
#include <processthreadsapi.h>
#define ALIGNED_MALLOC(alignment, size) _aligned_malloc(size, alignment)
#elif defined __APPLE__ || defined __linux__
#include <sys/syscall.h>
#include <unistd.h>
#define ALIGNED_MALLOC(alignment, size) aligned_alloc(alignment, size)
#endif
#include <stdlib.h>

#include "common.h"
#include "os_abstraction.h"

uint64_t
p64_gettid(void)
{
#if defined _WIN32
    return GetCurrentThreadId();
#elif defined __APPLE__
    return syscall(SYS_thread_selfid);
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
	ptr = ALIGNED_MALLOC(alignment, ROUNDUP(size, alignment));
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
