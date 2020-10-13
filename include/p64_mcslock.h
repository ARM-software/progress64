// Copyright (c) 2020 ARM Limited. All rights reserved.
//
// SPDX-License-Identifier: BSD-3-Clause

//MCS queue lock

#ifndef P64_MCSLOCK_H
#define P64_MCSLOCK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct p64_mcsnode
{
    struct p64_mcsnode *next;
    uint8_t wait;
} p64_mcsnode_t;

typedef p64_mcsnode_t *p64_mcslock_t;

//Initialise an MCS lock
void p64_mcslock_init(p64_mcslock_t *lock);

//Acquire an MCS lock
//'node' points to an uninitialized p64_mcsnode
void p64_mcslock_acquire(p64_mcslock_t *lock, p64_mcsnode_t *node);

//Release an MCS lock
//'node' must specify same node as used in matching acquire call
void p64_mcslock_release(p64_mcslock_t *lock, p64_mcsnode_t *node);

#ifdef __cplusplus
}
#endif

#endif
