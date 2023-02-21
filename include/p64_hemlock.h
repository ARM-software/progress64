// Copyright (c) 2023 ARM Limited. All rights reserved.
//
// SPDX-License-Identifier: BSD-3-Clause

//Hemlock queue lock
//See "Hemlock: Compact and Scalable Mutual Exclusion" by Dice & Kogan


#ifndef P64_HEMLOCK_H
#define P64_HEMLOCK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct p64_hemlock
{
    struct p64_hemlock **tail;
} p64_hemlock_t;

//Initialise a hemlock
void p64_hemlock_init(p64_hemlock_t *lock);

//Acquire a hemlock
void p64_hemlock_acquire(p64_hemlock_t *lock);

//Try to acquire a hemlock
bool p64_hemlock_try_acquire(p64_hemlock_t *lock);

//Release an hemlock
void p64_hemlock_release(p64_hemlock_t *lock);

#ifdef __cplusplus
}
#endif

#endif
