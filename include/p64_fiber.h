//Copyright (c) 2024, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

//Fibers with round robin scheduling
//Low overhead, <20 cycles for fiber yield (Arm N1)

#ifndef P64_FIBER_H
#define P64_FIBER_H

#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "p64_crosscall.h"

typedef struct p64_fiber
{
    p64_crosscall_t ctx;
    struct p64_fiber *nxt;
} p64_fiber_t;

struct p64_fiber_state
{
    uint32_t fcnt;
    uint32_t bcnt;
    p64_fiber_t *list;
    p64_fiber_t *cur;
    p64_fiber_t main;
};

extern _Thread_local struct p64_fiber_state p64_fiber_state;

//Spawn a fiber
//Fiber runs immediately so that it can read its arguments
void
p64_fiber_spawn(p64_fiber_t *,
		void (*func)(va_list *),
		void *stkbt,
		size_t stksz,
		...);

//Yield to next fiber
//Save context of this fiber
//Restore context of next fiber, resuming execution
static inline void
__attribute__((always_inline))
p64_fiber_yield(void)
{
    p64_fiber_t *out = p64_fiber_state.cur;
    p64_fiber_t *in = out->nxt;
    p64_fiber_state.cur = in; //Fiber executing after the cross jump
    p64_cross_call(0, &out->ctx, &in->ctx);
}

//Fiber exits and ceases to exist
void
__attribute__((noreturn))
p64_fiber_exit(void);

//Parent runs fibers until all fibers have exited
void
p64_fiber_run(void);

//Block fiber until all fibers have reached barrier
void
p64_fiber_barrier(void);

#endif
