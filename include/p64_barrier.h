//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

//Simple sense-reversal centralized thread barrier

#ifndef _P64_BARRIER_H
#define _P64_BARRIER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct p64_barrier
{
    uint32_t numthr;
    uint32_t waiting;
} p64_barrier_t;

//Initialise a synchronisation barrier
void p64_barrier_init(p64_barrier_t *br, uint32_t numthreads);

//Enter the barrier and wait until all (other) threads have also entered
//the barrier before leaving
//p64_barrier_wait() has release and acquire ordering
void p64_barrier_wait(p64_barrier_t *br);

#ifdef __cplusplus
}
#endif

#endif
