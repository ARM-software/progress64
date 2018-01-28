//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _P64_RWLOCK_H
#define _P64_RWLOCK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef uint32_t p64_rwlock_t;

void p64_rwlock_init(p64_rwlock_t *lock);
void p64_rwlock_acquire_rd(p64_rwlock_t *lock);
void p64_rwlock_release_rd(p64_rwlock_t *lock);
void p64_rwlock_acquire_wr(p64_rwlock_t *lock);
void p64_rwlock_release_wr(p64_rwlock_t *lock);

#ifdef __cplusplus
}
#endif

#endif
