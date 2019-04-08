//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdio.h>
#include "p64_pfrwlock.h"
#include "expect.h"

int main(void)
{
    p64_pfrwlock_t lock;
    p64_pfrwlock_init(&lock);
    p64_pfrwlock_acquire_rd(&lock);
    p64_pfrwlock_acquire_rd(&lock);
    p64_pfrwlock_release_rd(&lock);
    p64_pfrwlock_release_rd(&lock);
    p64_pfrwlock_acquire_wr(&lock);
    p64_pfrwlock_release_wr(&lock);

    printf("pfrwlock tests complete\n");
    return 0;
}
