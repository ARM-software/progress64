//Copyright (c) 2024, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "p64_fiber.h"
#include "err_hnd.h"

_Thread_local struct p64_fiber_state p64_fiber_state = { 0 }; //Initialise all elements to 0

static inline void
fiber_assert(void)
{
    uint32_t count = 0;
    p64_fiber_t *p = p64_fiber_state.list;
    if (p64_fiber_state.list != NULL)
    {
	count++;
	p64_fiber_t *q = p->nxt;
	while (p != q)
	{
	    count++;
	    q = q->nxt;
	}
    }
    (void)count;
    assert(count == p64_fiber_state.fcnt);
    assert((p64_fiber_state.fcnt != 0 && p64_fiber_state.list != NULL) ||
	   (p64_fiber_state.fcnt == 0 && p64_fiber_state.list == NULL));
}

//Remove element 'q'
static void
remove_element(p64_fiber_t *q)
{
    fiber_assert();
    p64_fiber_t *p = p64_fiber_state.list;
    while (p->nxt != q)
    {
	p = p->nxt;
    }
    assert(p->nxt == q);
    //Skip over 'q'
    p->nxt = q->nxt;
    p64_fiber_state.fcnt--;
    if (p64_fiber_state.fcnt != 0)
    {
	if (p64_fiber_state.list == q)
	{
	    p64_fiber_state.list = q->nxt;
	}
    }
    else
    {
	p64_fiber_state.list = NULL;
    }
    if (q != &p64_fiber_state.main)
    {
	q->nxt = NULL;
    }
    else
    {
	q->nxt = q;
    }
    fiber_assert();
}

//Insert element 'q' before 'p'
static void
insert_element_before(p64_fiber_t *p, p64_fiber_t *q)
{
    fiber_assert();
    if (p != NULL)
    {
	assert(p64_fiber_state.list != NULL);
	assert(p64_fiber_state.fcnt != 0);
	p64_fiber_t *b = p64_fiber_state.list;
	while (b->nxt != p)
	{
	    b = b->nxt;
	}
	assert(b->nxt == p);
	//'b' is before 'p'
	//Now insert 'q' after 'b'
	q->nxt = p;
	b->nxt = q;
	p64_fiber_state.fcnt++;
    }
    else
    {
	assert(p64_fiber_state.list == NULL);
	assert(p64_fiber_state.fcnt == 0);
	q->nxt = q;
	p64_fiber_state.list = q;
	p64_fiber_state.fcnt = 1;
    }
    fiber_assert();
} 

void
__attribute__((noreturn))
p64_fiber_exit(void)
{
    if (p64_fiber_state.cur == NULL || p64_fiber_state.cur == &p64_fiber_state.main)
    {
	report_error("fiber", "Non-fiber called fiber_exit()", NULL);
	abort();
    }
    //fiber_state.cur called fiber_exit()
    p64_fiber_t *out = p64_fiber_state.cur;
    p64_fiber_t *in = out->nxt;
    remove_element(p64_fiber_state.cur);
    //Check if there is another fiber to execute
    if (p64_fiber_state.fcnt != 0)
    {
	p64_fiber_state.cur = in;
	//Switch to next fiber
	p64_cross_call(0, &out->ctx, &in->ctx);
	abort();
    }
    else //No more fibers
    {
	//Return to main
	p64_fiber_state.cur = &p64_fiber_state.main;
	p64_cross_call(1, &out->ctx, &p64_fiber_state.main.ctx);
	abort();
    }
    //Should never get here
}

struct wrapper_args
{
    void (*ep)(va_list *);
    va_list *args;
};

static void
fiber_wrapper(struct wrapper_args *ptr)
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
    ptr->ep(ptr->args);
    p64_fiber_exit();
}

void
p64_fiber_spawn(p64_fiber_t *fbr,
	    void (*ep)(va_list *),
	    void *stkbt,
	    size_t stksz,
	    ...)
{
    //Late but not too late initialization of ptr to current context
    if (p64_fiber_state.cur == NULL)
    {
	p64_fiber_state.cur = &p64_fiber_state.main;
	p64_fiber_state.main.nxt = &p64_fiber_state.main;
    }
    fiber_assert();
    //Insert "main" to ensure fiber list is not empty
    insert_element_before(p64_fiber_state.list, &p64_fiber_state.main);
    //Ensure stack pointer is 16B aligned
    uintptr_t sp = ((uintptr_t)stkbt + stksz) & ~(uintptr_t)15;
    //Save user entrypoint and va_list pointer in wrapper args
    struct wrapper_args wargs;
    va_list args;
    va_start(args, stksz);
    wargs.args = &args;
    wargs.ep = ep;
    //Initialize context
    fbr->ctx.pc = (uintptr_t)fiber_wrapper;
    fbr->ctx.sp = sp;
    fbr->ctx.fp = 0;
    //Insert new fiber *before* current fiber
    insert_element_before(p64_fiber_state.cur, fbr);
    //Our own context is after new context so it will yield to us
    assert(fbr->nxt == p64_fiber_state.cur);
    //Call new fiber so that it can consume args before we return
    p64_fiber_t *saved = p64_fiber_state.cur;
    p64_fiber_state.cur = fbr;
    //Continue execution in new fiber
    p64_cross_call((uintptr_t)&wargs, &saved->ctx, &fbr->ctx);
    //And we are back
    p64_fiber_state.cur = saved;
    //Remove "main" from fiber list, it has served its purpose
    remove_element(&p64_fiber_state.main);
    va_end(args);
}

void
p64_fiber_run(void)
{
    fiber_assert();
    if (p64_fiber_state.cur != &p64_fiber_state.main && p64_fiber_state.cur != NULL)
    {
	report_error("fiber", "Fiber called fiber_run()", p64_fiber_state.cur);
	return;
    }
    if (p64_fiber_state.fcnt != 0)
    {
	p64_fiber_state.cur = p64_fiber_state.list;
	if (p64_fiber_state.cur != NULL)
	{
	    p64_cross_call(1, &p64_fiber_state.main.ctx, &p64_fiber_state.cur->ctx);
	}
	p64_fiber_state.cur = &p64_fiber_state.main;
    }
    //Else no fibers exist
}

void
p64_fiber_barrier(void)
{
    fiber_assert();
    if (p64_fiber_state.cur == NULL || p64_fiber_state.cur == &p64_fiber_state.main)
    {
	report_error("fiber", "Non-fiber called fiber_barrier()", NULL);
	return;
    }
    //Increment counter, one more fiber is waiting
    uint32_t me = p64_fiber_state.bcnt++;
    //Spin while not all fibers are waiting
    while (p64_fiber_state.bcnt != p64_fiber_state.fcnt)
    {
	//Yield so that other fibers can proceed and enter the barrier
	p64_fiber_yield();
    }
    //All fibers have arrived at the barrier, we can start leaving the while-loop
    //It is safe to read p64_fiber_state.fcnt before any fiber has returned
    bool is_first_to_leave = me == p64_fiber_state.fcnt - 1;
    //Yield so that other fibers can also leave the while-loop
    p64_fiber_yield();
    //All fibers have left the while-loop
    //First fiber to leave resets the barrier counter
    if (is_first_to_leave)
    {
	p64_fiber_state.bcnt = 0;
    }
    //Any fiber can now enter the barrier again
}
