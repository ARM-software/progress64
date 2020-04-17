//Copyright (c) 2020, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "p64_skiplock.h"

int main(void)
{
    p64_skiplock_t sl;
    p64_skiplock_init(&sl);//0-64
    p64_skiplock_acquire(&sl, 0);
    p64_skiplock_release(&sl, 0);//1-65
    p64_skiplock_skip(&sl, 1);//2-66
    p64_skiplock_acquire(&sl, 2);
    p64_skiplock_skip(&sl, 3);
    p64_skiplock_release(&sl, 2);//4-68
    p64_skiplock_acquire(&sl, 4);
    p64_skiplock_release(&sl, 4);//5-69
    p64_skiplock_skip(&sl, 69);//In range
    //p64_skiplock_skip(&sl, 70);//Not in range, will hang due to 1T
    p64_skiplock_skip(&sl, 7);//5-69
    p64_skiplock_skip(&sl, 6);//5-69
    p64_skiplock_skip(&sl, 8);//5-69
    p64_skiplock_acquire(&sl, 5);
    p64_skiplock_release(&sl, 5);//6-70
    p64_skiplock_skip(&sl, 70);

    printf("skiplock tests complete\n");
    return 0;
}
