//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

//Task fair reader/writer lock
//Task fairness interleaves reader and writer phases so avoids starvation

#ifndef P64_TFRWLOCK_H
#define P64_TFRWLOCK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct p64_tfrwlock p64_tfrwlock_t;

struct p64_tfrwlock
{
    union
    {
	uint32_t rdwr;
	struct
	{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	    uint16_t wr;//Bits 0..15
	    uint16_t rd;//Bits 16..31
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	    uint16_t rd;//Bits 16..31
	    uint16_t wr;//Bits 0..15
#else
#error
#endif
	};
    } enter;
    union
    {
	uint32_t rdwr;
	struct
	{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	    uint16_t wr;//Bits 0..15
	    uint16_t rd;//Bits 16..31
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	    uint16_t rd;//Bits 16..31
	    uint16_t wr;//Bits 0..15
#else
#error
#endif
	};
    } leave;
};

//Initialise a task fair reader/writer lock
void p64_tfrwlock_init(p64_tfrwlock_t *lock);

//Acquire a reader (shared) lock
//Block until no writer is in progress
void p64_tfrwlock_acquire_rd(p64_tfrwlock_t *lock);

//Release a reader lock
void p64_tfrwlock_release_rd(p64_tfrwlock_t *lock);

//Acquire a writer (exclusive) lock
//Block until earlier reader & writer locks have been released
void p64_tfrwlock_acquire_wr(p64_tfrwlock_t *lock, uint16_t *tkt);

//Release a writer lock
void p64_tfrwlock_release_wr(p64_tfrwlock_t *lock, uint16_t tkt);

#ifdef __cplusplus
}
#endif

#endif
