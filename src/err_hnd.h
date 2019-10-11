//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _ERR_HND_H
#define _ERR_HND_H

#include <inttypes.h>

void report_error(const char *module, const char *error, uintptr_t val);
#define report_error(a, b, c) report_error((a), (b), (uintptr_t)(c))

#endif
