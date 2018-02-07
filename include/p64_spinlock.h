//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _P64_SPINLOCK_H
#define _P64_SPINLOCK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef uint8_t p64_spinlock_t;

//Initialise a spin lock
void p64_spin_init(p64_spinlock_t *lock);

//Acquire a spin lock
void p64_spin_lock(p64_spinlock_t *lock);

//Release a spin lock
void p64_spin_unlock(p64_spinlock_t *lock);

//Release a spin lock when no writes to shared data have been made
void p64_spin_unlock_ro(p64_spinlock_t *lock);

#ifdef __cplusplus
}
#endif

#endif
