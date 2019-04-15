// Copyright (c) 2019 ARM Limited. All rights reserved.
//
// SPDX-License-Identifier: BSD-3-Clause

#ifndef _P64_RWCLHLOCK
#define _P64_RWCLHLOCK

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct p64_rwclhnode p64_rwclhnode_t;

typedef struct
{
    p64_rwclhnode_t *tail;
} p64_rwclhlock_t;

//Initialise a reader/writer CLH lock
void p64_rwclhlock_init(p64_rwclhlock_t *lock);

//Finish a reader/writer CLH lock
void p64_rwclhlock_fini(p64_rwclhlock_t *lock);

//Acquire a reader/writer CLH lock
//*nodep will be written with a pointer to a p64_rwclhnode_t, this memory must
//eventually be freed
void p64_rwclhlock_acquire_rd(p64_rwclhlock_t *lock, p64_rwclhnode_t **nodep);
void p64_rwclhlock_acquire_wr(p64_rwclhlock_t *lock, p64_rwclhnode_t **nodep);

//Release a reader/writer CLH lock
void p64_rwclhlock_release_rd(p64_rwclhnode_t **nodep);
void p64_rwclhlock_release_wr(p64_rwclhnode_t **nodep);

#ifdef __cplusplus
}
#endif

#endif
