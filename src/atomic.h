//Copyright (c) 2024-2025, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _ATOMIC_H
#define _ATOMIC_H

#include <stdint.h>

#include "arch.h"
#include "lockfree.h"
#include "verify.h"

#define atomic_load_n(loc, mo) \
({ \
    __typeof(loc) _loc = (loc); \
    __typeof(mo) _mo = (mo); \
    __typeof(*(loc)) _res =  __atomic_load_n(_loc, _mo); \
    VERIFY_SUSPEND(sizeof(*(loc)) | V_OP | V_AD | V_RE, "load", _loc, (intptr_t)_res, 0, 0, _mo); \
    _res; \
})

#define atomic_load_ptr atomic_load_n

#define atomic_store_n(loc, val, mo) \
({ \
    __typeof(loc) _loc = (loc); \
    __typeof(*(loc)) _val = (val); \
    __typeof(mo) _mo = (mo); \
    __atomic_store_n(_loc, _val, _mo); \
    VERIFY_SUSPEND(sizeof(*(loc)) | V_OP | V_AD | V_A1, "store", _loc, 0, (intptr_t)_val, 0, _mo); \
})

#define atomic_store_ptr atomic_store_n

#define atomic_fetch_add(loc, val, mo) \
({ \
    __typeof(loc) _loc = (loc); \
    __typeof(*(loc)) _val = (val); \
    __typeof(mo) _mo = (mo); \
    __typeof(*(loc)) _res = __atomic_fetch_add(_loc, _val, _mo); \
    VERIFY_SUSPEND(sizeof(*(loc)) | V_OP | V_AD | V_RE | V_A1, "fetch_add", _loc, _res, _val, 0, _mo); \
    _res; \
})

#define atomic_fetch_sub(loc, val, mo) \
({ \
    __typeof(loc) _loc = (loc); \
    __typeof(*(loc)) _val = (val); \
    __typeof(mo) _mo = (mo); \
    __typeof(*(loc)) _res = __atomic_fetch_sub(_loc, _val, _mo); \
    VERIFY_SUSPEND(sizeof(*(loc)) | V_OP | V_AD | V_RE | V_A1, "fetch_sub", _loc, _res, _val, 0, _mo); \
    _res; \
})

#define atomic_fetch_and(loc, val, mo) \
({ \
    __typeof(loc) _loc = (loc); \
    __typeof(*(loc)) _val = (val); \
    __typeof(mo) _mo = (mo); \
    __typeof(*(loc)) _res = __atomic_fetch_and(_loc, _val, _mo); \
    VERIFY_SUSPEND(sizeof(*(loc)) | V_OP | V_AD | V_RE | V_A1, "fetch_and", _loc, _res, _val, 0, _mo); \
    _res; \
})

#define atomic_fetch_or(loc, val, mo) \
({ \
    __typeof(loc) _loc = (loc); \
    __typeof(*(loc)) _val = (val); \
    __typeof(mo) _mo = (mo); \
    __typeof(*(loc)) _res = __atomic_fetch_or(_loc, _val, _mo); \
    VERIFY_SUSPEND(sizeof(*(loc)) | V_OP | V_AD | V_RE | V_A1, "fetch_or", _loc, _res, _val, 0, _mo); \
    _res; \
})

#define atomic_exchange_n(loc, swp, mo) \
({ \
    __typeof(loc) _loc = (loc); \
    __typeof(*(loc)) _swp = (swp); \
    __typeof(mo) _mo = (mo); \
    __typeof(*(loc)) _res =  __atomic_exchange_n(_loc, _swp, _mo); \
    VERIFY_SUSPEND(sizeof(*(loc)) | V_OP | V_AD | V_RE | V_A1, "exchange", _loc, (intptr_t)_res, (intptr_t)_swp, 0, _mo); \
    _res; \
})

#define atomic_exchange_ptr atomic_exchange_n

#define atomic_compare_exchange_n(loc, cmp, swp, mo_succ, mo_fail) \
({ \
    __typeof(loc) _loc = (loc); \
    __typeof(loc) _cmp = (cmp); \
    __typeof(*(loc)) _mem = *_cmp; (void)_mem; \
    __typeof(*(loc)) _swp = (swp); \
    __typeof(mo_succ) _mo_succ = (mo_succ); \
    __typeof(mo_fail) _mo_fail = (mo_fail); \
    int _res = \
    _Generic((loc), \
	uint16_t *: __atomic_compare_exchange_n, \
	uint32_t *: __atomic_compare_exchange_n, \
	uint64_t *: __atomic_compare_exchange_n, \
	__int128 *: lockfree_compare_exchange_16 \
	)(_loc, _cmp, _swp, false, _mo_succ, _mo_fail); \
    VERIFY_SUSPEND(sizeof(*(loc)) | V_OP | V_AD | V_RE | V_A1 | V_A2, "compare_exchange", _loc, _res, _mem, _swp, _mo_succ); \
    _res; \
})

//Need a special implementation of atomic_compare_exchange_ptr() due to need for cast
//from pointer to uintptr_t to __int128 in order to avoid cast of pointer to different size
#define atomic_compare_exchange_ptr(loc, cmp, swp, mo_succ, mo_fail) \
({ \
    __typeof(loc) _loc = (loc); \
    __typeof(loc) _cmp = (cmp); \
    __typeof(*(loc)) _mem = *_cmp; (void)_mem; \
    __typeof(*(loc)) _swp = (swp); \
    __typeof(mo_succ) _mo_succ = (mo_succ); \
    __typeof(mo_fail) _mo_fail = (mo_fail); \
    int _res = __atomic_compare_exchange_n(_loc, _cmp, _swp, false, _mo_succ, _mo_fail); \
    VERIFY_SUSPEND(sizeof(*(loc)) | V_OP | V_AD | V_RE | V_A1 | V_A2, "compare_exchange", _loc, _res, (uintptr_t)_mem, (uintptr_t)_swp, _mo_succ); \
    _res; \
})

//atomic_ldx() is load-to-monitor-before-WFE
//LDX() is defined by arch.h
#define atomic_ldx(loc, mo) \
({ \
    __typeof(loc) __loc = (loc); \
    __typeof(mo) _mo = (mo); \
    __typeof(*(loc)) _res = LDX(__loc, _mo); \
    VERIFY_SUSPEND(sizeof(*(loc)) | V_OP | V_AD | V_RE, "load", __loc, _res, 0, 0, _mo); \
    _res; \
})

//wait_until_equal() and wait_until_not_equal() macros that simplify programming

#define wait_until_equal(loc, val, mo) \
    ({ \
	__typeof(loc) _loc = (loc); \
	__typeof(*loc) _val = (val); \
	__typeof(mo) _mo = (mo); \
	while (atomic_ldx(_loc, _mo) != _val) \
	{ \
	    WFE(); \
	    VERIFY_YIELD(); \
	} \
    })

#define wait_until_equal_ptr(ptr, val, mo) \
({ \
    void **_ptr = (void **)(ptr); \
    wait_until_equal((uintptr_t *)_ptr, (uintptr_t)(val), (mo)); \
})

#define wait_until_not_equal(loc, val, mo) \
    ({ \
	__typeof(loc) _loc = (loc); \
	__typeof(*loc) _val = (val); \
	__typeof(mo) _mo = (mo); \
	__typeof(*loc) _mem; \
	while ((_mem = atomic_ldx(_loc, _mo)) == _val) \
	{ \
	    WFE(); \
	    VERIFY_YIELD(); \
	} \
	_mem; \
    })

#define wait_until_not_equal_ptr(ptr, val, mo) \
({ \
    void **_ptr = (void **)(ptr); \
    (__typeof(*(ptr))) wait_until_not_equal((uintptr_t *)_ptr, (uintptr_t)(val), (mo)); \
})

#define wait_until_equal_w_bkoff(loc, val, dly, mo) \
    ({ \
	__typeof(loc) _loc = (loc); \
	__typeof(*loc) _val = (val); \
	__typeof(*loc) _dly = (dly); \
	__typeof(mo) _mo = (mo); \
	while (atomic_ldx(_loc, _mo) != _val) \
	{ \
	    nano_delay(_dly); \
	    VERIFY_YIELD(); \
	} \
    })

#ifdef __ARM_FEATURE_ATOMICS

#define atomic_icas_n(loc, mo) \
({ \
    __typeof(loc) _loc = (loc); \
    __typeof(mo) _mo = (mo); \
    __typeof(*(loc)) _res = \
    _Generic((loc), \
	uint32_t *: icas4, \
	uint64_t *: icas8, \
	__int128 *: icas16 \
	)(_loc, _mo); \
    VERIFY_SUSPEND(sizeof(*(loc)) | V_OP | V_AD | V_RE, "icas", _loc, _res, 0, 0, _mo); \
    _res; \
})

#define atomic_cas_n(loc, cmp, swp, mo) \
({ \
    __typeof(loc) _loc = (loc); \
    __typeof(*(loc)) _cmp = (cmp); \
    __typeof(*(loc)) _swp = (swp); \
    __typeof(mo) _mo = (mo); \
    __typeof(*(loc)) _res = \
    _Generic((loc), \
	__int128 *: cas16 \
	)(_loc, _cmp, _swp, _mo); \
    VERIFY_SUSPEND(sizeof(*(loc)) | V_OP | V_AD | V_RE | V_A1 | V_A2, "cas", _loc, _res, _cmp, _swp, _mo); \
    _res; \
})

#endif

#endif
