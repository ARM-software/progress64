//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _P64_PFRWLOCK_H
#define _P64_PFRWLOCK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct p64_pfrwlock p64_pfrwlock_t;

struct p64_pfrwlock
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    union
    {
	uint64_t word;
	struct
	{
	    uint16_t enter_rd;//Bits 0..15
	    uint16_t pend_rd;
	    uint16_t leave_wr;
	    uint16_t enter_wr;//Bits 48..63
	};
    };
    uint16_t leave_rd;
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#else
#error
#endif
};

//Initialise a task fair reader/writer lock
void p64_pfrwlock_init(p64_pfrwlock_t *lock);

//Acquire a reader (shared) lock
//Block until no writer is in progress
void p64_pfrwlock_acquire_rd(p64_pfrwlock_t *lock);

//Release a reader lock
void p64_pfrwlock_release_rd(p64_pfrwlock_t *lock);

//Acquire a writer (exclusive) lock
//Block until earlier shared & exclusive locks have been released
void p64_pfrwlock_acquire_wr(p64_pfrwlock_t *lock);

//Release a writer lock
void p64_pfrwlock_release_wr(p64_pfrwlock_t *lock);

#ifdef __cplusplus
}
#endif

#endif
