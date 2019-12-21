//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

//Shared counters using per-thread stashes

#ifndef _P64_COUNTER_H
#define _P64_COUNTER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define P64_COUNTER_F_HP      0x0001 //Use hazard pointers (default QSBR)

typedef struct p64_cntdomain p64_cntdomain_t;

//Allocate a counter domain with space for 'ncounters' 64-bit counters
p64_cntdomain_t *p64_cntdomain_alloc(uint32_t ncounters, uint32_t flags);

//Free a counter domain
//No registered threads may remain
void p64_cntdomain_free(p64_cntdomain_t *cntd);

//Register a thread, allocate per-thread resources
void p64_cntdomain_register(p64_cntdomain_t *cntd);

//Unregister a thread, free any per-thread resources
//p64_cntdomain_unregister() uses the hazard pointer API
void p64_cntdomain_unregister(p64_cntdomain_t *cntd);

//Counter identifier (cntid)
typedef uint32_t p64_counter_t;

//Represents an invalid counter identifier
#define P64_COUNTER_INVALID 0

//Allocate a shared counter and return corresponding counter identifier
p64_counter_t p64_counter_alloc(p64_cntdomain_t *cntd);

//Free a shared counter
void p64_counter_free(p64_cntdomain_t *cntd, p64_counter_t cntid);

//Increment a shared 64-bit counter
void p64_counter_add(p64_cntdomain_t *cntd, p64_counter_t cntid, uint64_t val);

//Reset (to 0) a shared 64-bit counter
void p64_counter_reset(p64_cntdomain_t *cntd, p64_counter_t cntid);

//Read a shared 64-bit counter
//p64_counter_read() uses the hazard pointer API
uint64_t p64_counter_read(p64_cntdomain_t *cntd, p64_counter_t cntid);

#ifdef __cplusplus
}
#endif

#endif
