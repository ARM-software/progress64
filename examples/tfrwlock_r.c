//Copyright (c) 2018-2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdio.h>
#include "p64_tfrwlock_r.h"
#include "expect.h"

int main(void)
{
    p64_tfrwlock_r_t lock_A;
    p64_tfrwlock_r_t lock_B;
    p64_tfrwlock_r_init(&lock_A);
    p64_tfrwlock_r_init(&lock_B);

    EXPECT(lock_A.tfrwlock.enter.rdwr == lock_A.tfrwlock.leave.rdwr);
    p64_tfrwlock_r_acquire_rd(&lock_A);
    p64_tfrwlock_r_acquire_rd(&lock_A);
    p64_tfrwlock_r_release_rd(&lock_A);
    p64_tfrwlock_r_release_rd(&lock_A);
    EXPECT(lock_A.tfrwlock.enter.rdwr == lock_A.tfrwlock.leave.rdwr);

    p64_tfrwlock_r_acquire_wr(&lock_A);
    p64_tfrwlock_r_acquire_rd(&lock_A);
    p64_tfrwlock_r_acquire_wr(&lock_A);
    p64_tfrwlock_r_release_wr(&lock_A);
    p64_tfrwlock_r_release_rd(&lock_A);
    p64_tfrwlock_r_release_wr(&lock_A);
    EXPECT(lock_A.tfrwlock.enter.rdwr == lock_A.tfrwlock.leave.rdwr);

    EXPECT(lock_B.tfrwlock.enter.rdwr == lock_B.tfrwlock.leave.rdwr);
    p64_tfrwlock_r_acquire_rd(&lock_A);
    p64_tfrwlock_r_acquire_rd(&lock_B);
    p64_tfrwlock_r_acquire_rd(&lock_A);
    p64_tfrwlock_r_release_rd(&lock_A);
    p64_tfrwlock_r_release_rd(&lock_B);
    p64_tfrwlock_r_release_rd(&lock_A);

    p64_tfrwlock_r_acquire_rd(&lock_A);
    p64_tfrwlock_r_acquire_wr(&lock_B);
    p64_tfrwlock_r_acquire_rd(&lock_A);
    p64_tfrwlock_r_acquire_wr(&lock_B);
    p64_tfrwlock_r_release_wr(&lock_B);
    p64_tfrwlock_r_release_rd(&lock_A);
    p64_tfrwlock_r_release_wr(&lock_B);
    p64_tfrwlock_r_release_rd(&lock_A);

    p64_tfrwlock_r_acquire_wr(&lock_A);
    p64_tfrwlock_r_acquire_rd(&lock_A);
    p64_tfrwlock_r_release_rd(&lock_A);
    p64_tfrwlock_r_release_wr(&lock_A);
    EXPECT(lock_A.tfrwlock.enter.rdwr == lock_A.tfrwlock.leave.rdwr);
    EXPECT(lock_B.tfrwlock.enter.rdwr == lock_B.tfrwlock.leave.rdwr);

    printf("tfrwlock_r tests complete\n");
    return 0;
}
