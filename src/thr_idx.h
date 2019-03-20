//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _THR_IDX_H
#define _THR_IDX_H

#include <stdint.h>

int32_t p64_idx_alloc(void);
void p64_idx_free(int32_t idx);

#endif
