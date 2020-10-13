// Copyright (c) 2020 ARM Limited. All rights reserved.
//
// SPDX-License-Identifier: BSD-3-Clause

//Non-blocking CAS-N
//See "A practical multi-word compare-and-swap" by Harris, Fraser & Pratt

#ifndef P64_MCAS_H
#define P64_MCAS_H

#include <stdbool.h>
#include <stdint.h>
#include "p64_hazardptr.h"

#ifdef __cplusplus
extern "C"
{
#endif

//Generic element type.
//All pointers must be at least 4-byte aligned, 2 lsb are used internally
typedef void *p64_mcas_ptr_t;

//Pre-allocate 'count' descriptors for 'n'-location CAS and
//save in per-thread stash
void p64_mcas_init(uint32_t count, uint32_t n);

//Free all descriptors in per-thread stash
void p64_mcas_fini(void);

//Shared locations must be read using p64_mcas_read()
//Specify valid pointer to hazardptr variable or NULL for QSBR
p64_mcas_ptr_t p64_mcas_read(p64_mcas_ptr_t *loc, p64_hazardptr_t *hpp);

//Compare-and-swap on single location
bool p64_mcas_cas1(p64_mcas_ptr_t *loc,
		   p64_mcas_ptr_t exp,//Expected value
		   p64_mcas_ptr_t neu,//New value
		   bool use_hp);//Use hazard pointers or QSBR

//Compare-and-swap on (one or) multiple locations
bool p64_mcas_casn(uint32_t n,
		   p64_mcas_ptr_t *loc[n],
		   p64_mcas_ptr_t exp[n],//Expected values
		   p64_mcas_ptr_t neu[n],//New values
		   bool use_hp);//Use hazard pointers or QSBR

#ifdef __cplusplus
}
#endif

#endif
