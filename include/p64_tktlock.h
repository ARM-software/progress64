//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

//Ticket lock

#ifndef P64_TKTLOCK_H
#define P64_TKTLOCK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef uint32_t p64_tktlock_t;

void p64_tktlock_init(p64_tktlock_t *lock);

void p64_tktlock_acquire(p64_tktlock_t *lock);

void p64_tktlock_release(p64_tktlock_t *lock);

#ifdef __cplusplus
}
#endif

#endif
