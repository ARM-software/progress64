//Copyright (c) 2020, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

//Skiplock
//Create mutual exclusion with predefined ordering among threads
//This is similar to a ticket lock where the ticket is acquired separately
//A ticket can also be skipped in advance, potentially without blocking

#ifndef _P64_SKIPLOCK_H
#define _P64_SKIPLOCK_H

#include <assert.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct
{
    _Alignas(2 * sizeof(uintptr_t))
    uint32_t cur;
#if __SIZEOF_POINTER__ == 8 && __SIZEOF_INT128__ == 16
    unsigned __int128 mask:96;
#else
    uint32_t mask;
#endif
} p64_skiplock_t;

static_assert(sizeof(p64_skiplock_t) == 2 * sizeof(uintptr_t),
	     "sizeof(p64_skiplock_t) == 2 * sizeof(uintptr_t)");

//Initialise a skiplock
void p64_skiplock_init(p64_skiplock_t *sl);

//Acquire the lock in the order specified by the ticket
void p64_skiplock_acquire(p64_skiplock_t *sl, uint32_t tkt);

//Release the lock
void p64_skiplock_release(p64_skiplock_t *sl, uint32_t tkt);

//Skip the specified ticket
//This is a non-blocking call if the ticket is within the range starting from
//the current ticket being served
//If the ticket is beyond the range, this call will block until ticket is
//within range
void p64_skiplock_skip(p64_skiplock_t *sl, uint32_t tkt);

#ifdef __cplusplus
}
#endif

#endif
