//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _BUILD_CONFIG_H
#define _BUILD_CONFIG_H

#define USE_SPLIT_PRODCONS
//#define USE_SPLIT_HEADTAIL

//Use load/store exclusive directly
#ifdef __aarch64__
#define USE_LDXSTX
#endif

//Use ARMv8 Wait For Event mechanism which generally improves performance
#ifdef __aarch64__
#define USE_WFE
#endif

//Use DMB;STR which is faster than STLR on e.g. Cortex-A57
#ifdef __aarch64__
#define USE_DMB
#endif

#define CACHE_LINE 64
#define MAXTHREADS 128

#define MAXTIMERS 8192

#endif
