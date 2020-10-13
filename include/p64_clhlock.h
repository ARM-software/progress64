// Copyright (c) 2018 ARM Limited. All rights reserved.
//
// SPDX-License-Identifier: BSD-3-Clause

//CLH queue lock

#ifndef P64_CLHLOCK_H
#define P64_CLHLOCK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct p64_clhnode p64_clhnode_t;

typedef struct
{
    p64_clhnode_t *tail;
} p64_clhlock_t;

//Initialise a CLH lock
void p64_clhlock_init(p64_clhlock_t *lock);

//Finish a CLH lock
void p64_clhlock_fini(p64_clhlock_t *lock);

//Acquire a CLH lock
//*nodep will be written with a pointer to a p64_clhnode_t object, this
//object must eventually be freed
void p64_clhlock_acquire(p64_clhlock_t *lock, p64_clhnode_t **nodep);

//Release a CLH lock
void p64_clhlock_release(p64_clhnode_t **nodep);

#ifdef __cplusplus
}
#endif

#endif
