//Copyright (c) 2024, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _ATOMIC_H
#define _ATOMIC_H

#include <stdint.h>

#include "arch.h"
#include "lockfree.h"
#include "verify.h"

#define atomic_load_n(loc, mm) \
({ \
    __typeof(*(loc)) _res = \
    _Generic((loc), \
	uint8_t  *: __atomic_load_n, \
	uint16_t *: __atomic_load_n, \
	uint32_t *: __atomic_load_n, \
	uint64_t *: __atomic_load_n, \
	void *   *: __atomic_load_n \
	)((loc), (mm)); \
    VERIFY_SUSPEND(V_OP | V_AD | V_RE, "load", loc, (intptr_t)_res, 0, 0); \
    _res; \
})

#define atomic_load_ptr(ptr, mm) \
     ((__typeof(*(ptr))) atomic_load_n((void **)(ptr), (mm)))

#define atomic_store_n(loc, val, mm) \
({ \
    __typeof(*(loc)) _val = (val); \
    _Generic((loc), \
	uint8_t  *: __atomic_store_n, \
	uint16_t *: __atomic_store_n, \
	uint32_t *: __atomic_store_n, \
	uint64_t *: __atomic_store_n, \
	void *   *: __atomic_store_n \
	)((loc), _val, (mm)); \
    VERIFY_SUSPEND(V_OP | V_AD | V_A1, "store", loc, 0, _val, 0); \
})

#define atomic_store_ptr(ptr, swp, mm) \
    atomic_store_n((void **)(ptr), (void *)(swp), (mm))

#define atomic_fetch_add(loc, val, mm) \
({ \
    __typeof(*(loc)) _val = (val); \
    __typeof(*(loc)) _res = \
    _Generic((loc), \
	uint32_t *: __atomic_fetch_add, \
	uint64_t *: __atomic_fetch_add \
	)((loc), _val, (mm)); \
    VERIFY_SUSPEND(V_OP | V_AD | V_RE | V_A1, "fetch_add", loc, _res, _val, 0); \
    _res; \
})

#define atomic_fetch_sub(loc, val, mm) \
({ \
    __typeof(*(loc)) _val = (val); \
    __typeof(*(loc)) _res = \
    _Generic((loc), \
	uint32_t *: __atomic_fetch_sub, \
	uint64_t *: __atomic_fetch_sub \
	)((loc), _val, (mm)); \
    VERIFY_SUSPEND(V_OP | V_AD | V_RE | V_A1, "fetch_sub", loc, _res, _val, 0); \
    _res; \
})

#define atomic_fetch_and(loc, val, mm) \
({ \
    __typeof(*(loc)) _val = (val); \
    __typeof(*(loc)) _res = \
    _Generic((loc), \
	uint32_t *: __atomic_fetch_and, \
	uint64_t *: __atomic_fetch_and \
	)((loc), _val, (mm)); \
    VERIFY_SUSPEND(V_OP | V_AD | V_RE | V_A1, "fetch_and", loc, _res, _val, 0); \
    _res; \
})

#define atomic_fetch_or(loc, val, mm) \
({ \
    __typeof(*(loc)) _val = (val); \
    __typeof(*(loc)) _res = \
    _Generic((loc), \
	uint32_t *: __atomic_fetch_or, \
	uint64_t *: __atomic_fetch_or \
	)((loc), _val, (mm)); \
    VERIFY_SUSPEND(V_OP | V_AD | V_RE | V_A1, "fetch_or", loc, _res, _val, 0); \
    _res; \
})

#define atomic_exchange_n(loc, val, mo) \
({ \
    __typeof(*(loc)) _val = (val); \
    __typeof(*(loc)) _res = \
    _Generic((loc), \
	uint32_t *: __atomic_exchange_n, \
	uint64_t *: __atomic_exchange_n, \
	void *   *: __atomic_exchange_n \
	)((loc), _val, (mo)); \
    VERIFY_SUSPEND(V_OP | V_AD | V_RE | V_A1, "exchange", loc, _res, _val, 0); \
    _res; \
})

#define atomic_exchange_ptr(ptr, swp, mm) \
     ((__typeof(*(ptr))) atomic_exchange_n((void **)(ptr), (void *)(swp), (mm)))

#define atomic_compare_exchange_n(loc, cmp, swp, mo_succ, mo_fail) \
({ \
    __typeof(loc) _cmp = (cmp); \
    __typeof(*(loc)) _mem = *_cmp; (void)_mem; \
    __typeof(*(loc)) _swp = (swp); \
    int _res = \
    _Generic((loc), \
	uint16_t *: __atomic_compare_exchange_n, \
	uint32_t *: __atomic_compare_exchange_n, \
	uint64_t *: __atomic_compare_exchange_n, \
	void *   *: __atomic_compare_exchange_n, \
	__int128 *: lockfree_compare_exchange_16 \
	)((loc), _cmp, _swp, false, (mo_succ), (mo_fail)); \
    VERIFY_SUSPEND(V_OP | V_AD | V_RE | V_A1 | V_A2, "compare_exchange", loc, _res, _mem, _swp); \
    _res; \
})

#define atomic_compare_exchange_ptr(ptr, cmp, swp, mo_succ, mo_fail) \
    atomic_compare_exchange_n((void **)(ptr), (void **)cmp, (void *)(swp), (mo_succ), (mo_fail))

//atomic_ldx() is load-to-monitor-before-WFE
//LDX() is defined by arch.h
#define atomic_ldx(loc, mm) \
({ \
    __typeof(loc) __loc = (loc); \
    __typeof(*(loc)) _res = LDX(__loc, (mm)); \
    VERIFY_SUSPEND(V_OP | V_AD | V_RE, "load", __loc, _res, 0, 0); \
    _res; \
})

//wait_until_equal() and wait_until_not_equal() macros that simplify programming

#define wait_until_equal(loc, val, mm) \
    ({ \
	__typeof(loc) _loc = (loc); \
	__typeof(*loc) _val = (val); \
	while (atomic_ldx(_loc, (mm)) != _val) \
	{ \
	    WFE(); \
	    VERIFY_YIELD(); \
	} \
    })

#define wait_until_equal_ptr(ptr, val, mm) \
({ \
    void **_ptr = (void **)(ptr); \
    wait_until_equal((uintptr_t *)_ptr, (uintptr_t)(val), (mm)); \
})

#define wait_until_not_equal(loc, val, mm) \
    ({ \
	__typeof(loc) _loc = (loc); \
	__typeof(*loc) _val = (val); \
	__typeof(*loc) _mem; \
	while ((_mem = atomic_ldx(_loc, (mm))) == _val) \
	{ \
	    WFE(); \
	    VERIFY_YIELD(); \
	} \
	_mem; \
    })

#define wait_until_not_equal_ptr(ptr, val, mm) \
({ \
    void **_ptr = (void **)(ptr); \
    (__typeof(*(ptr))) wait_until_not_equal((uintptr_t *)_ptr, (uintptr_t)(val), (mm)); \
})

#define wait_until_equal_w_bkoff(loc, val, dly, mm) \
    ({ \
	__typeof(loc) _loc = (loc); \
	__typeof(*loc) _val = (val); \
	__typeof(*loc) _dly = (dly); \
	while (atomic_ldx(_loc, (mm)) != _val) \
	{ \
	    nano_delay(_dly); \
	    VERIFY_YIELD(); \
	} \
    })

#ifdef __ARM_FEATURE_ATOMICS

#define atomic_icas_n(loc, mm) \
({ \
    __typeof(*(loc)) _res = \
    _Generic((loc), \
	uint32_t *: icas4, \
	uint64_t *: icas8, \
	__int128 *: icas16 \
	)((loc), (mm)); \
    VERIFY_SUSPEND(V_OP | V_AD | V_RE, "icas", loc, _res, 0, 0); \
    _res; \
})

#define atomic_cas_n(loc, cmp, swp, mm) \
({ \
    __typeof(*(loc)) _cmp = (cmp); \
    __typeof(*(loc)) _swp = (swp); \
    __typeof(*(loc)) _res = \
    _Generic((loc), \
	__int128 *: cas16 \
	)((loc), _cmp, _swp, (mm)); \
    VERIFY_SUSPEND(V_OP | V_AD | V_RE | V_A1 | V_A2, "cas", loc, _res, _cmp, _swp); \
    _res; \
})

#endif

#endif
