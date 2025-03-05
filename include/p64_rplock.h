// Copyright (c) 2025 ARM Limited. All rights reserved.
//
// SPDX-License-Identifier: BSD-3-Clause

//Reciprocating (queue) lock
//See "Reciprocating Locks" by Dice & Kogan

#ifndef P64_RPLOCK_H
#define P64_RPLOCK_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct p64_rpnode
{
    struct p64_rpnode *gate;
    struct p64_rpnode *succ;
    struct p64_rpnode *eos;
} p64_rpnode_t;

typedef struct p64_rplock
{
    p64_rpnode_t *arrivals;
} p64_rplock_t;

//Initialise an rplock
void p64_rplock_init(p64_rplock_t *lock);

//Acquire an rplock
void p64_rplock_acquire(p64_rplock_t *lock, p64_rpnode_t *node);

//Try to acquire an rplock
bool p64_rplock_try_acquire(p64_rplock_t *lock, p64_rpnode_t *node);

//Release an rplock
void p64_rplock_release(p64_rplock_t *lock, p64_rpnode_t *node);

#ifdef __cplusplus
}
#endif

#endif
