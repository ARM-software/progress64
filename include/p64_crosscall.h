//Copyright (c) 2024-2025, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

//The cross-call function switches between different stackful contexts

#ifndef P64_CROSSCALL_H
#define P64_CROSSCALL_H

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
    uintptr_t pc;
    uintptr_t sp;
    uintptr_t fp;
} p64_crosscall_t;

__attribute__((always_inline))
static inline intptr_t
p64_cross_call(intptr_t arg, p64_crosscall_t *out, p64_crosscall_t *in)
{
static_assert(offsetof(p64_crosscall_t, pc) == 0, "offsetof(p64_crosscall_t, pc) == 0");
static_assert(offsetof(p64_crosscall_t, sp) == 8, "offsetof(p64_crosscall_t, sp) == 8");
static_assert(offsetof(p64_crosscall_t, fp) == 16, "offsetof(p64_crosscall_t, fp) == 16");
#ifdef __aarch64__
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
#else
    register intptr_t         rdi __asm("rdi") = arg;//1st argument
    register p64_crosscall_t *rsi __asm("rsi") = out;//2nd argument
    register p64_crosscall_t *rdx __asm("rdx") = in;//3rd argument
    __asm __volatile("movq %%rbp,16(%1)\n" //Save old FP
		     "movq %%rsp,8(%1)\n" //Save old SP
		     "lea  1f(%%rip),%%rax\n"//Read PC of "1" label
		     "movq %%rax,0(%1)\n" //Save old PC
		     "movq 16(%2),%%rbp\n" //Load and restore new FP
		     "movq 8(%2),%%rsp\n"  //Load and restore new SP
		     "movq 0(%2),%%rax\n" //Load new PC
		     "jmp *%%rax\n" //Jump to (restore) PC
		     "1: endbr64\n"//Indirect jump landing pad
		     : "+r" (rdi), "+r" (rsi), "+r" (rdx)
		     :
		     : "rax", "rbx", "rcx", /*rdx*/ /*rsi*/ /*rdi*/ /*rsp*/ /*rbp*/
		       "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
#if defined __AVX__ || defined __AVX2__
		       "ymm0", "ymm1", "ymm2", "ymm3", "ymm4", "ymm5", "ymm6", "ymm7",
		       "ymm8", "ymm9", "ymm10", "ymm11", "ymm12", "ymm13", "ymm14", "ymm15",
#elif defined __SSE__ || defined __SSE2__
		       "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
		       "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15",
#endif
		       "cc", "memory");
    return rdi;//arg from other side
#endif
}

#endif
