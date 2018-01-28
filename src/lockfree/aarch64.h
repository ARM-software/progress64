//Copyright (c) 2017, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _LOCKFREE_AARCH64_H
#define _LOCKFREE_AARCH64_H

#include <stdbool.h>
#include "common.h"

#include "ldxstx.h"

#define HAS_ACQ(mo) ((mo) != __ATOMIC_RELAXED && (mo) != __ATOMIC_RELEASE)
#define HAS_RLS(mo) ((mo) == __ATOMIC_RELEASE || (mo) == __ATOMIC_ACQ_REL || (mo) == __ATOMIC_SEQ_CST)
#define LDX_MO(mo) (HAS_ACQ((mo)) ? __ATOMIC_ACQUIRE : __ATOMIC_RELAXED)
#define STX_MO(mo) (HAS_RLS((mo)) ? __ATOMIC_RELEASE : __ATOMIC_RELAXED)

#ifdef __ARM_FEATURE_QRDMX //Feature only available in v8.1a and beyond
static inline __int128 casp(__int128 *var, __int128 old, __int128 neu, int mo)
{
    if (mo == __ATOMIC_RELAXED)
    {
	__asm __volatile("casp %0, %H0, %1, %H1, [%2]"
		: "+r" (old)
		: "r" (neu), "r" (var)
		: "memory");
    }
    else if (mo == __ATOMIC_ACQUIRE)
    {
	__asm __volatile("caspa %0, %H0, %1, %H1, [%2]"
		: "+r" (old)
		: "r" (neu), "r" (var)
		: "memory");
    }
    else if (mo == __ATOMIC_ACQ_REL)
    {
	__asm __volatile("caspal %0, %H0, %1, %H1, [%2]"
		: "+r" (old)
		: "r" (neu), "r" (var)
		: "memory");
    }
    else if (mo == __ATOMIC_RELEASE)
    {
	__asm __volatile("caspl %0, %H0, %1, %H1, [%2]"
		: "+r" (old)
		: "r" (neu), "r" (var)
		: "memory");
    }
    else
    {
	abort();
    }
    return old;
}
#endif

ALWAYS_INLINE
static inline bool lockfree_compare_exchange_16(register __int128 *var, __int128 *exp, register __int128 neu, bool weak, int mo_success, int mo_failure)
{
#ifdef __ARM_FEATURE_QRDMX //Feature only available in v8.1a and beyond
    (void)weak; (void)mo_failure;
    __int128 old, expected = *exp;
    old = casp(var, expected, neu, mo_success);
    *exp = old;//Always update, atomically read value
    return old == expected;
#else
    (void)weak;//Always do strong CAS or we can't perform atomic read
    (void)mo_failure;//Ignore memory ordering for failure, memory order for
    //success must be stronger or equal
    int ldx_mo = LDX_MO(mo_success);
    int stx_mo = STX_MO(mo_success);
    register __int128 old, expected = *exp;
    __asm __volatile("" ::: "memory");
    do
    {
	//Atomicity of LDX16 is not guaranteed
	old = ldx128(var, ldx_mo);
	//Must write back neu or old to verify atomicity of LDX16
    }
    while (UNLIKELY(stx128(var, old == expected ? neu : old, stx_mo)));
    *exp = old;//Always update, atomically read value
    return old == expected;
#endif
}

ALWAYS_INLINE
static inline bool lockfree_compare_exchange_16_frail(register __int128 *var, __int128 *exp, register __int128 neu, bool weak, int mo_success, int mo_failure)
{
#ifdef __ARM_FEATURE_QRDMX //Feature only available in v8.1a and beyond
    (void)weak; (void)mo_failure;
    __int128 old, expected = *exp;
    old = casp(var, expected, neu, mo_success);
    *exp = old;//Always update, atomically read value
    return old == expected;
#else
    (void)weak;//Weak CAS and non-atomic load on failure
    (void)mo_failure;//Ignore memory ordering for failure, memory order for
    //success must be stronger or equal
    int ldx_mo = LDX_MO(mo_success);
    int stx_mo = STX_MO(mo_success);
    register __int128 expected = *exp;
    __asm __volatile("" ::: "memory");
    //Atomicity of LDX16 is not guaranteed
    register __int128 old = ldx128(var, ldx_mo);
    if (LIKELY(old == expected && !stx128(var, neu, stx_mo)))
    {
	//Right value and STX succeeded
	__asm __volatile("" ::: "memory");
	return 1;
    }
    __asm __volatile("" ::: "memory");
    //Wrong value or STX failed
    *exp = old;//Old possibly torn value (OK for 'frail' flavour)
    return 0;//Failure, *exp updated
#endif
}

ALWAYS_INLINE
static inline __int128 lockfree_load_16(__int128 *var, int mo)
{
    __int128 old = *var;//Possibly torn read
    //Do CAS to ensure atomicity
    //Either CAS succeeds (writing back the same value)
    //Or CAS fails and returns the old value (atomic read)
    (void)lockfree_compare_exchange_16(var, &old, old, /*weak=*/false, mo, mo);
    return old;
}

ALWAYS_INLINE
static inline void lockfree_store_16(__int128 *var, __int128 neu, int mo)
{
#ifdef __ARM_FEATURE_QRDMX //Feature only available in v8.1a and beyond
    __int128 old, expected;
    do
    {
	expected = *var;
	old = casp(var, expected, neu, mo);
    }
    while (old != expected);
#else
    int ldx_mo = __ATOMIC_ACQUIRE;
    int stx_mo = STX_MO(mo);
    do
    {
	(void)ldx128(var, ldx_mo);
    }
    while (UNLIKELY(stx128(var, neu, stx_mo)));
#endif
}

ALWAYS_INLINE
static inline __int128 lockfree_exchange_16(__int128 *var, __int128 neu, int mo)
{
#ifdef __ARM_FEATURE_QRDMX //Feature only available in v8.1a and beyond
    __int128 old, expected;
    do
    {
	expected = *var;
	old = casp(var, expected, neu, mo);
    }
    while (old != expected);
    return old;
#else
    int ldx_mo = LDX_MO(mo);
    int stx_mo = STX_MO(mo);
    register __int128 old;
    do
    {
	old = ldx128(var, ldx_mo);
    }
    while (UNLIKELY(stx128(var, neu, stx_mo)));
    return old;
#endif
}

ALWAYS_INLINE
static inline __int128 lockfree_fetch_and_16(__int128 *var, __int128 mask, int mo)
{
#ifdef __ARM_FEATURE_QRDMX //Feature only available in v8.1a and beyond
    __int128 old, expected;
    do
    {
	expected = *var;
	old = casp(var, expected, expected & mask, mo);
    }
    while (old != expected);
    return old;
#else
    int ldx_mo = LDX_MO(mo);
    int stx_mo = STX_MO(mo);
    register __int128 old;
    do
    {
	old = ldx128(var, ldx_mo);
    }
    while (UNLIKELY(stx128(var, old & mask, stx_mo)));
    return old;
#endif
}

ALWAYS_INLINE
static inline __int128 lockfree_fetch_or_16(__int128 *var, __int128 mask, int mo)
{
#ifdef __ARM_FEATURE_QRDMX //Feature only available in v8.1a and beyond
    __int128 old, expected;
    do
    {
	expected = *var;
	old = casp(var, expected, expected | mask, mo);
    }
    while (old != expected);
    return old;
#else
    int ldx_mo = LDX_MO(mo);
    int stx_mo = STX_MO(mo);
    register __int128 old;
    do
    {
	old = ldx128(var, ldx_mo);
    }
    while (UNLIKELY(stx128(var, old | mask, stx_mo)));
    return old;
#endif
}

#endif
