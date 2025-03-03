//Copyright (c) 2024, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <assert.h>
#include <stdlib.h>

#include "p64_coroutine.h"
#include "err_hnd.h"

_Thread_local struct p64_coroutine_state p64_coro_state = { 0 }; //Initialise all elements to 0

//Called when a coroutine returns
//May also be called explicitly from anywhere in a coroutine
void
__attribute__((noreturn))
p64_coro_return(intptr_t arg)
{
    if (p64_coro_state.current == &p64_coro_state.main)
    {
	report_error("coroutine", "p64_coro_return() called from non-coroutine", p64_coro_state.current);
	abort();
    }
    //Continue execution in parent
    (void)p64_coro_suspend(arg);
    //Parent resumed us again!
    report_error("coroutine", "Resume of ceased coroutine", p64_coro_state.current);
    abort();
}

struct wrapper_args
{
    intptr_t (*ep)(va_list *);
    va_list *args;
};

static void
coro_wrapper(struct wrapper_args *ptr)
{
#ifdef __aarch64__
#if defined __APPLE__ && defined __GNUC__ && !defined __clang__
    //Real GCC on macOS (e.g. Brew GCC-13) seems not to handle .cfi_undefined well
#else
    //End of callstack magic
    __asm __volatile(".cfi_undefined x30\n"
		     "mov x29,#0\n"
		     "mov x30,#0\n");
#endif
#endif
    uintptr_t ret = ptr->ep(ptr->args);
    p64_coro_return(ret);
}

intptr_t
p64_coro_spawn(p64_coroutine_t *cr,
	       intptr_t (*ep)(va_list *),
	       void *stkbt,
	       size_t stksz,
	       ...)
{
    //Late but not too late initialization of ptr to current context
    if (p64_coro_state.current == NULL)
    {
	p64_coro_state.current = &p64_coro_state.main;
    }
    //Ensure stack pointer is 16B aligned
    uintptr_t sp = ((uintptr_t)stkbt + stksz) & ~(uintptr_t)15;
#ifdef __x86_64__
    sp -= 8; //Hack to ensure stack pointer (rsp) later is 16-byte aligned
#endif
    //Save user entrypoint and va_list pointer in wrapper args
    struct wrapper_args wargs;
    va_list args;
    va_start(args, stksz);
    wargs.args = &args;
    wargs.ep = ep;
    //Initialize context
    cr->pc = (uintptr_t)coro_wrapper;
    cr->sp = sp;
    cr->fp = 0;
    //Continue execution in coroutine
    uintptr_t arg = p64_coro_resume(cr, (uintptr_t)&wargs);
    //When coroutine yields first time, args become invalid
    va_end(args);
    return arg;
}
