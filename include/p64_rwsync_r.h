//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _P64_RWSYNC_R_H
#define _P64_RWSYNC_R_H

#include <stdint.h>
#include "p64_rwsync.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define P64_RWSYNC_INVALID_TID -1

typedef struct
{
    p64_rwsync_t rwsync;
    int32_t owner;
    int32_t count;
} p64_rwsync_r_t;

//Initialise a recursive read/write synchroniser
void p64_rwsync_r_init(p64_rwsync_r_t *sync);

//Acquire a synchroniser for reading
//Block until no write (by other thread) is in progress
//Not allowed to call acquire-read when a write by the same thread is in
//progress, protected data is in unknown state and the call cannot block
//waiting for the write to complete
//Recursive acquire-read calls allowed for the same synchroniser
p64_rwsync_t p64_rwsync_r_acquire_rd(const p64_rwsync_r_t *sync, int32_t tid);

//Release a read synchroniser
//Return false if a write has occurred or is in progress
//This means any read data may be inconsistent and the operation should be
//restarted
bool p64_rwsync_r_release_rd(const p64_rwsync_r_t *sync, p64_rwsync_t prv);

//Acquire a synchroniser for writing
//Block until earlier writes have completed
//Recursive acquire-write calls allowed for the same synchroniser
void p64_rwsync_r_acquire_wr(p64_rwsync_r_t *sync, int32_t tid);

//Release a write synchroniser
void p64_rwsync_r_release_wr(p64_rwsync_r_t *sync);

#ifdef __cplusplus
}
#endif

#endif
