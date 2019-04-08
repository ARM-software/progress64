//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _P64_TKTLOCK_H
#define _P64_TKTLOCK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct
{
    uint16_t enter, leave;
} p64_tktlock_t;

void p64_tktlock_init(p64_tktlock_t *lock);

void p64_tktlock_acquire(p64_tktlock_t *lock, uint16_t *tkt);

void p64_tktlock_release(p64_tktlock_t *lock, uint16_t tkt);

#ifdef __cplusplus
}
#endif

#endif
