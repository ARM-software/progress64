//Copyright (c) 2024, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

//The cross jump function switches between different stackful contexts

#ifndef P64_CROSSJUMP_H
#define P64_CROSSJUMP_H

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
    uintptr_t pc;
    uintptr_t sp;
    uintptr_t fp;
} p64_crosscall_t;

static inline intptr_t
p64_cross_call(intptr_t arg, p64_crosscall_t *out, p64_crosscall_t *in)
{
#ifdef __aarch64__
static_assert(offsetof(p64_crosscall_t, pc) == 0, "offsetof(p64_crosscall_t, pc) == 0");
static_assert(offsetof(p64_crosscall_t, sp) == 8, "offsetof(p64_crosscall_t, sp) == 8");
static_assert(offsetof(p64_crosscall_t, fp) == 16, "offsetof(p64_crosscall_t, fp) == 16");
    register intptr_t         x0 __asm("x0") = arg;
    register p64_crosscall_t *x1 __asm("x1") = out;
    register p64_crosscall_t *x2 __asm("x2") = in;
    __asm __volatile("str fp,[%1,#16]\n" //Save old FP
		     "mov x4,sp\n" //Read old SP
		     "adr x3,1f\n" //Read PC of "1" label
		     "stp x3,x4,[%1]\n" //Save old PC and old SP
		     "ldp x3,x4,[%2]\n" //Load new PC and SP
		     "ldr fp,[%2,#16]\n" //Load new FP
		     "mov sp,x4\n" //Restore SP
		     "br x3\n" //Jump to (restore) PC
		     ".align 4\n"
		     "1: hint #0x24\n" //BTI J - indirect jump landing pad
		     : "+r" (x0), "+r" (x1), "+r" (x2)
		     :
		     :             "x3", "x4", "x5", "x6", "x7",
		       "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
#ifdef __APPLE__
		       //x18 is reserved by macOS
		       "x16", "x17",/*x18*/ "x19", "x20", "x21", "x22", "x23",
#else
		       "x16", "x17", "x18", "x19", "x20", "x21", "x22", "x23",
#endif
		       "x24", "x25", "x26", "x27", "x28", /*fp*/ "x30", /*sp*/
#if defined __ARM_FP || defined __ARM_NEON
		       "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7",
		       "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15",
		       "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23",
		       "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31",
#endif
		       "cc", "memory");
    return x0;//arg from other side
#endif
}

#endif
