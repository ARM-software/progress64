//Copyright (c) 2016, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _LDXSTX_H
#define _LDXSTX_H

#include <stdint.h>
#include <stdlib.h>

#ifndef __aarch64__
#error
#endif

/******************************************************************************
 * ARMv8/A64 load/store exclusive primitives
 *****************************************************************************/

static inline uint8_t ldx8(uint8_t *var, int mm)
{
    uint8_t old;
    if (mm == __ATOMIC_ACQUIRE)
    __asm volatile("ldaxrb %w0, [%1]"
                   : "=&r" (old)
                   : "r" (var)
                   : "memory");
    else if (mm == __ATOMIC_RELAXED)
    __asm volatile("ldxrb %w0, [%1]"
                   : "=&r" (old)
                   : "r" (var)
                   : );
    else
	abort();
    return old;
}

static inline uint16_t ldx16(uint16_t *var, int mm)
{
    uint16_t old;
    if (mm == __ATOMIC_ACQUIRE)
    __asm volatile("ldaxrh %w0, [%1]"
                   : "=&r" (old)
                   : "r" (var)
                   : "memory");
    else if (mm == __ATOMIC_RELAXED)
    __asm volatile("ldxrh %w0, [%1]"
                   : "=&r" (old)
                   : "r" (var)
                   : );
    else
	abort();
    return old;
}

static inline uint32_t ldx32(uint32_t *var, int mm)
{
    uint32_t old;
    if (mm == __ATOMIC_ACQUIRE)
    __asm volatile("ldaxr %w0, [%1]"
                   : "=&r" (old)
                   : "r" (var)
                   : "memory");
    else if (mm == __ATOMIC_RELAXED)
    __asm volatile("ldxr %w0, [%1]"
                   : "=&r" (old)
                   : "r" (var)
                   : );
    else
	abort();
    return old;
}

//Return 0 on success, 1 on failure
static inline uint32_t stx32(uint32_t *var, uint32_t neu, int mm)
{
    uint32_t ret;
    if (mm == __ATOMIC_RELEASE)
    __asm volatile("stlxr %w0, %w1, [%2]"
                   : "=&r" (ret)
                   : "r" (neu), "r" (var)
                   : "memory");
    else if (mm == __ATOMIC_RELAXED)
    __asm volatile("stxr %w0, %w1, [%2]"
                   : "=&r" (ret)
                   : "r" (neu), "r" (var)
                   : );
    else
	abort();
    return ret;
}

static inline uint64_t ldx64(uint64_t *var, int mm)
{
    uint64_t old;
    if (mm == __ATOMIC_ACQUIRE)
    __asm volatile("ldaxr %0, [%1]"
                   : "=&r" (old)
                   : "r" (var)
                   : "memory");
    else if (mm == __ATOMIC_RELAXED)
    __asm volatile("ldxr %0, [%1]"
                   : "=&r" (old)
                   : "r" (var)
                   : );
    else
	abort();
    return old;
}

//Return 0 on success, 1 on failure
static inline uint32_t stx64(uint64_t *var, uint64_t neu, int mm)
{
    uint32_t ret;
    if (mm == __ATOMIC_RELEASE)
    __asm volatile("stlxr %w0, %1, [%2]"
                   : "=&r" (ret)
                   : "r" (neu), "r" (var)
                   : "memory");
    else if (mm == __ATOMIC_RELAXED)
    __asm volatile("stxr %w0, %1, [%2]"
                   : "=&r" (ret)
                   : "r" (neu), "r" (var)
                   : );
    else
	abort();
    return ret;
}

static inline __int128 ldx128(__int128 *var, int mm)
{
    __int128 old;
    if (mm == __ATOMIC_ACQUIRE)
    __asm volatile("ldaxp %0, %H0, [%1]"
                   : "=&r" (old)
                   : "r" (var)
                   : "memory");
    else if (mm == __ATOMIC_RELAXED)
    __asm volatile("ldxp %0, %H0, [%1]"
                   : "=&r" (old)
                   : "r" (var)
                   : );
    else
	abort();
    return old;
}

//Return 0 on success, 1 on failure
static inline uint32_t stx128(__int128 *var, __int128 neu, int mm)
{
    uint32_t ret;
    if (mm == __ATOMIC_RELEASE)
    __asm volatile("stlxp %w0, %1, %H1, [%2]"
                   : "=&r" (ret)
                   : "r" (neu), "r" (var)
                   : "memory");
    else if (mm == __ATOMIC_RELAXED)
    __asm volatile("stxp %w0, %1, %H1, [%2]"
                   : "=&r" (ret)
                   : "r" (neu), "r" (var)
                   : );
    else
	abort();
    return ret;
}

#endif
