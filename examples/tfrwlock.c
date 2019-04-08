//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdio.h>
#include "p64_tfrwlock.h"
#include "expect.h"

int main(void)
{
    uint16_t tkt;
    p64_tfrwlock_t lock;
    p64_tfrwlock_init(&lock);
    p64_tfrwlock_acquire_rd(&lock);
    p64_tfrwlock_acquire_rd(&lock);
    p64_tfrwlock_release_rd(&lock);
    p64_tfrwlock_release_rd(&lock);
    p64_tfrwlock_acquire_wr(&lock, &tkt);
    p64_tfrwlock_release_wr(&lock, tkt);

    printf("tfrwlock tests complete\n");
    return 0;
}
