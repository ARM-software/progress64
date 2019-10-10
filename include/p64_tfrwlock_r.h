//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

//Recursive version of task-fair reader/writer lock

#ifndef _P64_TFRWLOCK_R_H
#define _P64_TFRWLOCK_R_H

#include <stdbool.h>
#include <stdint.h>
#include "p64_tfrwlock.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct
{
    p64_tfrwlock_t tfrwlock;
    uint64_t owner;
} p64_tfrwlock_r_t;

//Initialise a recursive read/write lock
void p64_tfrwlock_r_init(p64_tfrwlock_r_t *lock);

//Acquire a tfrwlock for reading
//Block until no write is in progress (unless its own write)
//Recursive acquire-read calls allowed for the same tfrwlock
//Can acquire read lock when write lock already acquired for same lock
void p64_tfrwlock_r_acquire_rd(p64_tfrwlock_r_t *lock);

//Release a read lock
void p64_tfrwlock_r_release_rd(p64_tfrwlock_r_t *lock);

//Acquire a tfrwlock for writing
//Block until earlier reads & writes have completed
//Recursive acquire-write calls allowed for the same tfrwlock
//Cannot upgrade read lock to write lock
void p64_tfrwlock_r_acquire_wr(p64_tfrwlock_r_t *lock);

//Release a write lock
void p64_tfrwlock_r_release_wr(p64_tfrwlock_r_t *lock);

#ifdef __cplusplus
}
#endif

#endif
