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

    EXPECT(lock_A.rwlock == 0);
    //Try-acquire read lock that is free => success
    EXPECT(p64_rwlock_r_try_acquire_rd(&lock_A) == true);
    //Try-acquire write lock with present readers => failure
    EXPECT(p64_rwlock_r_try_acquire_wr(&lock_A) == false);
    p64_rwlock_r_acquire_rd(&lock_A);
    //Try-acquire write lock with present readers => failure
    EXPECT(p64_rwlock_r_try_acquire_wr(&lock_A) == false);
    //Try-acquire read lock with present readers => success
    EXPECT(p64_rwlock_r_try_acquire_rd(&lock_A) == true);
    p64_rwlock_r_release_rd(&lock_A);
    p64_rwlock_r_release_rd(&lock_A);
    //Try-acquire write lock with present readers => failure
    EXPECT(p64_rwlock_r_try_acquire_wr(&lock_A) == false);
    p64_rwlock_r_release_rd(&lock_A);

    EXPECT(lock_A.rwlock == 0);
    //Try-acquire write lock that is free => success
    EXPECT(p64_rwlock_r_try_acquire_wr(&lock_A) == true);
    p64_rwlock_r_release_wr(&lock_A);

    p64_rwlock_r_acquire_wr(&lock_A);
    //Try-acquire write lock when lock is owned by us => success
    EXPECT(p64_rwlock_r_try_acquire_wr(&lock_A) == true);
    //Try-acquire read lock with present writer (upgrade) => success
    EXPECT(p64_rwlock_r_try_acquire_rd(&lock_A) == true);
    p64_rwlock_r_release_rd(&lock_A);
    p64_rwlock_r_acquire_wr(&lock_A);
    p64_rwlock_r_release_wr(&lock_A);
    p64_rwlock_r_release_wr(&lock_A);
    p64_rwlock_r_release_wr(&lock_A);

    EXPECT(lock_A.rwlock == 0);
    p64_rwlock_r_acquire_rd(&lock_A);
    //Try-acquire write lock with present readers (upgrade) => failure
    EXPECT(p64_rwlock_r_try_acquire_wr(&lock_A) == false);
    //Try-acquire read lock with present readers => success
    EXPECT(p64_rwlock_r_try_acquire_rd(&lock_A) == true);
    p64_rwlock_r_release_rd(&lock_A);
    p64_rwlock_r_release_rd(&lock_A);

    EXPECT(lock_A.rwlock == 0);
    EXPECT(lock_B.rwlock == 0);
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

    p64_rwlock_r_acquire_wr(&lock_A);
    p64_rwlock_r_acquire_rd(&lock_A);
    p64_rwlock_r_release_rd(&lock_A);
    p64_rwlock_r_release_wr(&lock_A);
    EXPECT(lock_A.rwlock == 0);
    EXPECT(lock_B.rwlock == 0);

    printf("rwlock_r tests complete\n");
    return 0;
}
