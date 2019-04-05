//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdio.h>
#include "p64_tfrwlock.h"
#include "expect.h"

int main(void)
{
    p64_tfrwlock_t lock;
    p64_tfrwlock_init(&lock);
    p64_tfrwlock_acquire_rd(&lock);
    //EXPECT(lock == 0x0001000000000000UL);
    p64_tfrwlock_acquire_rd(&lock);
    //EXPECT(lock == 0x0002000000000000UL);
    p64_tfrwlock_release_rd(&lock);
    //EXPECT(lock == 0x0002000100000000U);
    p64_tfrwlock_release_rd(&lock);
    //EXPECT(lock == 0x0002000200000000U);
    p64_tfrwlock_acquire_wr(&lock);
    //EXPECT(lock == 0x0002000200000001U);
    p64_tfrwlock_release_wr(&lock);
    //EXPECT(lock == 0x0002000200010001U);

    printf("tfrwlock tests complete\n");
    return 0;
}
