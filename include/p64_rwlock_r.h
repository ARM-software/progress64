//Copyright (c) 2018-2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

//Recursive version of reader/writer lock

#ifndef P64_RWLOCK_R_H
#define P64_RWLOCK_R_H

#include <stdbool.h>
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
//Block until no write is in progress (unless its own write)
//Recursive acquire-read calls allowed for the same rwlock
//Can acquire read lock when write lock already acquired for same lock
void p64_rwlock_r_acquire_rd(p64_rwlock_r_t *lock);

//Try to acquire a rwlock for reading
//Return false immediately instead of blocking for earlier write to complete
bool p64_rwlock_r_try_acquire_rd(p64_rwlock_r_t *lock);
//Recursive acquire-read calls allowed for the same rwlock
//Can acquire read lock when write lock already acquired for same lock

//Release a read lock
void p64_rwlock_r_release_rd(p64_rwlock_r_t *lock);

//Acquire a rwlock for writing
//Block until earlier reads & writes have completed
//Recursive acquire-write calls allowed for the same rwlock
//Cannot upgrade read lock to write lock
void p64_rwlock_r_acquire_wr(p64_rwlock_r_t *lock);

//Try to acquire a rwlock for writing
//Return false immediately instead of blocking until earlier reads & writes
//have completed
bool p64_rwlock_r_try_acquire_wr(p64_rwlock_r_t *lock);
//Recursive acquire-write calls allowed for the same rwlock
//Cannot upgrade read lock to write lock

//Release a write lock
void p64_rwlock_r_release_wr(p64_rwlock_r_t *lock);

#ifdef __cplusplus
}
#endif

#endif
