//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _P64_RWSYNC_H
#define _P64_RWSYNC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef uint32_t p64_rwsync_t;

//Initialise a read/write synchroniser
void p64_rwsync_init(p64_rwsync_t *sync);

//Acquire a synchroniser for reading
//Block until no write is in progress
p64_rwsync_t p64_rwsync_acquire_rd(const p64_rwsync_t *sync);

//Release a read synchroniser
//Return false if a write has occured or is in progress
//This means any read data may be inconsistent and the operation should be
//restarted
bool p64_rwsync_release_rd(const p64_rwsync_t *sync, p64_rwsync_t prv);

//Acquire a synchroniser for writing
//Block until earlier writes have completed
void p64_rwsync_acquire_wr(p64_rwsync_t *sync);

//Release a write synchroniser
void p64_rwsync_release_wr(p64_rwsync_t *sync);

//Perform an atomic read of the associated data
//Will block for concurrent writes
void p64_rwsync_read(p64_rwsync_t *sync,
		     void *dst,
		     const void *data,
		     size_t len);

//Perform an atomic write of the associated data
//Will block for concurrent writes
void p64_rwsync_write(p64_rwsync_t *sync,
		      const void *src,
		      void *data,
		      size_t len);

#ifdef __cplusplus
}
#endif

#endif
