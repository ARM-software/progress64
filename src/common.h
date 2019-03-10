//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _COMMON_H
#define _COMMON_H

//Compiler hints
#define ALWAYS_INLINE __attribute__((always_inline))
#ifdef __clang__
#define UNROLL_LOOPS __attribute__((opencl_unroll_hint(8)))
#else
#define UNROLL_LOOPS __attribute__((optimize("unroll-loops")))
#endif
#define INIT_FUNCTION __attribute__((constructor))
#define LIKELY(x)    __builtin_expect(!!(x), 1)
#define UNLIKELY(x)  __builtin_expect(!!(x), 0)

//Hardware hints
#define PREFETCH_FOR_READ(ptr) __builtin_prefetch((ptr), 0, 0)
#define PREFETCH_FOR_WRITE(ptr) __builtin_prefetch((ptr), 1, 0)

#define ALIGNED(x) __attribute__((__aligned__(x)))

#define ROUNDUP_POW2(x) \
    ({ \
         unsigned long _x = (x); \
         _x > 1 ?  (1UL << (__SIZEOF_LONG__ * __CHAR_BIT__ - __builtin_clzl(_x - 1UL))) : 1; \
     })


#define ROUNDUP(a, b) \
    ({                                          \
        __typeof__ (a) tmp_a = (a);             \
        __typeof__ (b) tmp_b = (b);             \
        ((tmp_a + tmp_b - 1) / tmp_b) * tmp_b;  \
    })

#define MIN(a, b) \
    ({                                          \
        __typeof__ (a) tmp_a = (a);             \
        __typeof__ (b) tmp_b = (b);             \
        tmp_a < tmp_b ? tmp_a : tmp_b;          \
    })

#define MAX(a, b) \
    ({                                          \
        __typeof__ (a) tmp_a = (a);             \
        __typeof__ (b) tmp_b = (b);             \
        tmp_a > tmp_b ? tmp_a : tmp_b;          \
    })

#define IS_POWER_OF_TWO(n) \
    ({                                            \
	__typeof__ (n) tmp_n = (n);               \
	tmp_n != 0 && (tmp_n & (tmp_n - 1)) == 0; \
    })

#define SWAP(_a, _b) \
{ \
    __typeof__ (_a) _t; \
    _t = _a; \
    _a = _b; \
    _b = _t; \
}

#endif
