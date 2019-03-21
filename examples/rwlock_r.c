//Copyright (c) 2018-2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdio.h>
#include <string.h>
#include "p64_rwlock_r.h"
#include "expect.h"

int main(void)
{
    p64_rwlock_r_t lock_A;
    p64_rwlock_r_t lock_B;
    p64_rwlock_r_init(&lock_A);
    p64_rwlock_r_init(&lock_B);

    p64_rwlock_r_acquire_rd(&lock_A);
    p64_rwlock_r_acquire_rd(&lock_A);
    p64_rwlock_r_release_rd(&lock_A);
    p64_rwlock_r_release_rd(&lock_A);

    p64_rwlock_r_acquire_wr(&lock_A);
    p64_rwlock_r_acquire_wr(&lock_A);
    p64_rwlock_r_release_wr(&lock_A);
    p64_rwlock_r_release_wr(&lock_A);

    p64_rwlock_r_acquire_rd(&lock_A);
    p64_rwlock_r_release_rd(&lock_A);

    p64_rwlock_r_acquire_rd(&lock_A);
    p64_rwlock_r_acquire_rd(&lock_B);
    p64_rwlock_r_acquire_rd(&lock_A);
    p64_rwlock_r_release_rd(&lock_A);
    p64_rwlock_r_release_rd(&lock_B);
    p64_rwlock_r_release_rd(&lock_A);

    p64_rwlock_r_acquire_rd(&lock_A);
    p64_rwlock_r_acquire_wr(&lock_B);
    p64_rwlock_r_acquire_rd(&lock_A);
    p64_rwlock_r_acquire_wr(&lock_B);
    p64_rwlock_r_release_wr(&lock_B);
    p64_rwlock_r_release_rd(&lock_A);
    p64_rwlock_r_release_wr(&lock_B);
    p64_rwlock_r_release_rd(&lock_A);

    printf("rwlock_r tests complete\n");
    return 0;
}
