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
    __int128 res;
    __int128 arg1;
    __int128 arg2;
    uint32_t memo;
};

//Bits 0..7 used by size of data
#define V_OP 0x0100 //oper present
#define V_AD 0x0200 //addr present
#define V_RE 0x0400 //res present
#define V_A1 0x0800 //arg1 present
#define V_A2 0x1000 //arg2 present
#define V_STR 0x2000 //Print addr as a string
#define V_FORCE 0x4000 //Force yield to other thread
#define V_ABORT 0x8000 //Abort execution

#ifdef VERIFY

#include "p64_coroutine.h"

#define VERIFY_SUSPEND(fm, op, ad, re, a1, a2, mo) p64_coro_suspend((intptr_t)&(struct ver_file_line){.file = __FILE__, .line = __LINE__, .fmt = (fm), .oper = (op), .addr = (ad), .res = (__int128)(re), .arg1 = (__int128)(a1), .arg2 = (__int128)(a2), .memo = (mo) })
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

#define VERIFY_SUSPEND(fm, op, ad, re, a1, a2, mo) ((void)(fm),(void)(op),(void)(ad),(void)(re),(void)(a1),(void)(a2),(void)(mo))
#define VERIFY_YIELD() (void)0
#define VERIFY_ERROR(msg) (void)0
#define VERIFY_ASSERT(exp) (void)0

#endif

#endif
