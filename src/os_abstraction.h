//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _OS_ABSTRACTION_H
#define _OS_ABSTRACTION_H

#include <stddef.h>
#include <stdint.h>

#define INVALID_TID (~0UL)

uint64_t p64_gettid(void);

void *p64_malloc(size_t size, size_t alignment);

void p64_mfree(void *ptr);

#endif
