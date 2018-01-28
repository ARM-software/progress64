//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _P64_BARRIER_H
#define _P64_BARRIER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct
{
    uint32_t numthr;
    uint32_t waiting;
} p64_barrier_t;

void p64_barrier_init(p64_barrier_t *br, uint32_t numthreads);
void p64_barrier_wait(p64_barrier_t *br);

#ifdef __cplusplus
}
#endif

#endif
