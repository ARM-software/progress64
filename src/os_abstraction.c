//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include "os_abstraction.h"

#ifdef _WIN32
#include <processthreadsapi.h>
#elif defined __APPLE__ || defined __linux__
#include <sys/syscall.h>
#include <unistd.h>
#endif

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