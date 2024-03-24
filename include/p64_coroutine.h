//Copyright (c) 2024, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

//Symmetric and asymmetric stackful coroutines
//Low overhead, <40 cycles for coroutine resume/suspend (Arm N1)

#ifndef P64_COROUTINE_H
#define P64_COROUTINE_H

#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#include "p64_crosscall.h"

typedef p64_crosscall_t p64_coroutine_t;

struct p64_coroutine_state
{
    p64_coroutine_t *parent;
    p64_coroutine_t *current;
    p64_coroutine_t main;
};

extern _Thread_local struct p64_coroutine_state p64_coro_state;

//Parent spawns a coroutine
//Coroutine runs immediately so that it can read its arguments
intptr_t
p64_coro_spawn(p64_coroutine_t *cr,
	       intptr_t (*func)(va_list *),
	       void *stkbt,
	       size_t stksz,
	       ...);

//Suspend caller and resume execution in child coroutine
//Pass argument to child coroutine, returned from p64_coro_suspend call
static inline intptr_t
__attribute__((always_inline))
p64_coro_resume(p64_coroutine_t *cr, intptr_t arg)
{
    p64_coroutine_t *parent = p64_coro_state.parent;
    p64_coroutine_t *current = p64_coro_state.current;
    p64_coro_state.parent = current;
    p64_coro_state.current = cr;
    arg = p64_cross_call(arg, current, cr);
    p64_coro_state.parent = parent;
    return arg;
}

//Suspend calling coroutine and resume execution in parent
//Return argument to parent, returned from p64_coro_resume call
static inline intptr_t
__attribute__((always_inline))
p64_coro_suspend(intptr_t arg)
{
    p64_coroutine_t *parent = p64_coro_state.parent;
    p64_coroutine_t *current = p64_coro_state.current;
    p64_coro_state.current = parent;
    //Parent will update p64_coro_state.parent
    arg = p64_cross_call(arg, current, parent);
    return arg;
}

//Coroutine switches to other coroutine
static inline intptr_t
__attribute__((always_inline))
p64_coro_switch(p64_coroutine_t *cr, intptr_t arg)
{
    p64_coroutine_t *current = p64_coro_state.current;
    p64_coro_state.current = cr;
    arg = p64_cross_call(arg, current, cr);
    return arg;
}

void
__attribute__((noreturn))
p64_coro_return(intptr_t arg);

#ifdef __cplusplus
}
#endif

#endif
