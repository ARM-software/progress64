//Copyright (c) 2018-2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _P64_RWLOCK_R_H
#define _P64_RWLOCK_R_H

#include <stdint.h>
#include "p64_rwlock.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct
{
    p64_rwlock_t rwlock;
    uint64_t owner;
} p64_rwlock_r_t;

//Initialise a recursive read/write lock
void p64_rwlock_r_init(p64_rwlock_r_t *lock);

//Acquire a rwlock for reading
//Block until no write is in progress
//Not allowed to call acquire-read when the lock has already been acquired
//for write, protected data is in unknown state and the call cannot block
//waiting for the write to complete
//Recursive acquire-read calls allowed for the same rwlock
void p64_rwlock_r_acquire_rd(p64_rwlock_r_t *lock);

//Release a read lock
void p64_rwlock_r_release_rd(p64_rwlock_r_t *lock);

//Acquire a rwlock for writing
//Block until earlier reads & writes have completed
//Recursive acquire-write calls allowed for the same rwlock
void p64_rwlock_r_acquire_wr(p64_rwlock_r_t *lock);

//Release a write lock
void p64_rwlock_r_release_wr(p64_rwlock_r_t *lock);

#ifdef __cplusplus
}
#endif

#endif
