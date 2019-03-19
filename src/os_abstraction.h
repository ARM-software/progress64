//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _OS_ABSTRACTION_H
#define _OS_ABSTRACTION_H

#include <stdint.h>

#define INVALID_TID (~0UL)

uint64_t p64_gettid(void);

#endif
