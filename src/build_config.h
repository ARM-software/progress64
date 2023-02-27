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

#ifdef __arm__
#define CACHE_LINE 32
#define MAXTHREADS 16
#else
#define CACHE_LINE 64
#define MAXTHREADS 128
#endif
#define MAXTIMERS 8192

#endif
