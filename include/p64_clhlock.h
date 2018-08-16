// Copyright (c) 2018 ARM Limited. All rights reserved.
//
// SPDX-License-Identifier: BSD-3-Clause

#ifndef _P64_CLHLOCK
#define _P64_CLHLOCK

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct p64_clhnode
{
    struct p64_clhnode *prev;
    uint32_t wait;
} p64_clhnode_t;

typedef struct
{
    p64_clhnode_t node;
    p64_clhnode_t *tail __attribute__((__aligned__(64)));
} p64_clhlock_t;

//Initialise a CLH lock
void p64_clhlock_init(p64_clhlock_t *lock);

//Acquire a CLH lock
void p64_clhlock_acquire(p64_clhlock_t *lock, p64_clhnode_t *node);

//Release a CLH lock
void p64_clhlock_release(p64_clhnode_t **nodep);

#ifdef __cplusplus
}
#endif

#endif
