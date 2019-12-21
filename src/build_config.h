//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _BUILD_CONFIG_H
#define _BUILD_CONFIG_H

#define USE_SPLIT_PRODCONS
//#define USE_SPLIT_HEADTAIL

//Enable hazard pointers with zero references to invoke QSBR
//#define HP_ZEROREF_QSBR

//Use load/store exclusive directly
#ifdef __aarch64__
#ifndef __ARM_FEATURE_ATOMICS
//ARMv8.0 only has exclusives, use them directly for custom atomic operations
#define USE_LDXSTX
#endif
#endif

//Use ARMv8 Wait For Event mechanism which generally improves performance
#ifdef __aarch64__
#define USE_WFE
#endif

//Use DMB;STR which is faster than STLR on e.g. Cortex-A57
#ifdef __aarch64__
#define USE_DMB
#endif

#ifdef __aarch64__
//Use PREFETCH_ATOMIC() before __atomic calls when not generating ARMv8.1 LSE
//instructions (i.e. __atomic calls implemented using exclusives)
#ifndef __ARM_FEATURE_ATOMICS
#define PREFETCH_ATOMIC(p) __builtin_prefetch((p), 1, 3);
#endif
//Use PREFETCH_LDXSTX() before explicit exclusives (ldx/stx) usage
#define PREFETCH_LDXSTX(p) __builtin_prefetch((p), 1, 3);
#endif

//Default null definitions
#ifndef PREFETCH_ATOMIC
#define PREFETCH_ATOMIC(p) (void)(p)
#endif
#ifndef PREFETCH_LDXSTX
#define PREFETCH_LDXSTX(p) (void)(p)
#endif

#ifdef __arm__
#define CACHE_LINE 32
#define MAXTHREADS 16
#else
#define CACHE_LINE 64
#define MAXTHREADS 128
#endif
#define MAXTIMERS 8192

#endif
