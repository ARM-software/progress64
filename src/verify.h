//Copyright (c) 2024-2025, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _VERIFY_H
#define _VERIFY_H

struct ver_funcs
{
    const char *name;
    void (*init)(uint32_t numthreads);
    void (*exec)(uint32_t id);
    void (*fini)(uint32_t numthreads);
};

struct ver_file_line
{
    const char *file;
    intptr_t line;
    uint32_t fmt;
    const char *oper;
    const void *addr;
    intptr_t res;
    intptr_t arg1;
    intptr_t arg2;
};

#define V_OP 1 //oper present
#define V_AD 2 //addr present
#define V_RE 4 //res present
#define V_A1 8 //arg1 present
#define V_A2 16 //arg2 present
#define V_FORCE 32 //Force yield to other thread
#define V_ABORT 64 //Abort execution
#define V_STR 128 //Print addr as a string

#ifdef VERIFY

#include "p64_coroutine.h"

#define VERIFY_SUSPEND(fm, op, ad, re, a1, a2) p64_coro_suspend((intptr_t)&(struct ver_file_line){.file = __FILE__, .line = __LINE__, .fmt = (fm), .oper = (op), .addr = (ad), .res = (intptr_t)(re), .arg1 = (intptr_t)(a1), .arg2 = (intptr_t)(a2) })
#define VERIFY_YIELD() p64_coro_suspend((intptr_t)&(struct ver_file_line){.file = __FILE__, .line = __LINE__, .fmt = V_FORCE | V_OP, .oper = "force" })
#define VERIFY_ERROR(msg) p64_coro_suspend((intptr_t)&(struct ver_file_line){.file = __FILE__, .line = __LINE__, .fmt = V_OP | V_STR | V_ABORT, .oper = "error", .addr = (msg) })

#define VER_HASHSTR(s) #s
#define VER_STR(s) VER_HASHSTR(s)

#define VERIFY_ASSERT(exp) \
{ \
    if (!(exp)) \
	p64_coro_suspend((intptr_t)&(struct ver_file_line){.file = __FILE__, .line = __LINE__, .fmt = V_OP | V_STR | V_ABORT, .oper = "failed", .addr = VER_STR(exp) }); \
}

extern uint32_t verify_id;

#else

#define VERIFY_SUSPEND(fm, op, ad, re, a1, a2) ((void)(fm),(void)(op),(void)(ad),(void)(re),(void)(a1),(void)(a2))
#define VERIFY_YIELD() (void)0
#define VERIFY_ERROR(msg) (void)0
#define VERIFY_ASSERT(exp) (void)0

#endif

#endif
