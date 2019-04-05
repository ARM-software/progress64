//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _P64_TFRWLOCK_H
#define _P64_TFRWLOCK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct p64_tfrwlock p64_tfrwlock_t;

struct p64_tfrwlock
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    union
    {
	uint32_t word;
	struct
	{
	    uint16_t wr_ticket;//Bits 0..15
	    uint16_t rd_enter;//Bits 16..31
	};
    };
    uint16_t wr_serving;
    uint16_t rd_leave;
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#else
#error
#endif
};

//Initialise a task fair reader/writer lock
void p64_tfrwlock_init(p64_tfrwlock_t *lock);

//Acquire a reader (shared) lock
//Block until no writer is in progress
void p64_tfrwlock_acquire_rd(p64_tfrwlock_t *lock);

//Release a reader lock
void p64_tfrwlock_release_rd(p64_tfrwlock_t *lock);

//Acquire a writer (exclusive) lock
//Block until earlier shared & exclusive locks have been released
void p64_tfrwlock_acquire_wr(p64_tfrwlock_t *lock);

//Release a writer lock
void p64_tfrwlock_release_wr(p64_tfrwlock_t *lock);

#ifdef __cplusplus
}
#endif

#endif
