//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

//Reader/writer lock with writer preference

#ifndef _P64_RWLOCK_H
#define _P64_RWLOCK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef uint32_t p64_rwlock_t;

//Initialise a read/write lock
void p64_rwlock_init(p64_rwlock_t *lock);

//Acquire a reader (shared) lock
//Block until no writer is in progress
void p64_rwlock_acquire_rd(p64_rwlock_t *lock);

//Release a reader lock
void p64_rwlock_release_rd(p64_rwlock_t *lock);

//Acquire a writer (exclsuive) lock
//Block until earlier readers & writer locks have been released
void p64_rwlock_acquire_wr(p64_rwlock_t *lock);

//Try to acquire a reader lock
//Return false immediately instead of blocking until earlier write has completed
bool p64_rwlock_try_acquire_rd(p64_rwlock_t *lock);

//Try to acquire a writer lock
//Return false immediately instead of blocking until earlier reads & writes
//have completed
bool p64_rwlock_try_acquire_wr(p64_rwlock_t *lock);

//Release a writer lock
void p64_rwlock_release_wr(p64_rwlock_t *lock);

#ifdef __cplusplus
}
#endif

#endif
