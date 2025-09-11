//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _BUILD_CONFIG_H
#define _BUILD_CONFIG_H

#define USE_SPLIT_PRODCONS

//Enable hazard pointers with zero references to invoke QSBR
//#define HP_ZEROREF_QSBR

//Use ARMv8 Wait For Event mechanism which generally improves performance
#ifdef __aarch64__
#define USE_WFE
#endif

#define CACHE_LINE 64
#define MAXTHREADS 128
#define MAXTIMERS 8192

#endif
