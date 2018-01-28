//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _LOCKFREE_H
#define _LOCKFREE_H

#if defined __aarch64__

#include "lockfree/aarch64.h"

#elif defined __x86_64__

#include "lockfree/x86-64.h"

#else

#error Unsupported architecture

#endif

#endif
