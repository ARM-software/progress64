//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdio.h>
#include "p64_rwlock.h"
#include "expect.h"

int main(void)
{
    p64_rwlock_t lock;
    p64_rwlock_init(&lock);
    p64_rwlock_acquire_rd(&lock);
    EXPECT(lock == 1);
    //Try-acquire read lock with readers => success
    EXPECT(p64_rwlock_try_acquire_rd(&lock) == true);
    EXPECT(lock == 2);
    //Try-acquire write lock with readers => failure
    EXPECT(p64_rwlock_try_acquire_wr(&lock) == false);
    p64_rwlock_acquire_rd(&lock);
    EXPECT(lock == 3);
    p64_rwlock_release_rd(&lock);
    EXPECT(lock == 2);
    p64_rwlock_release_rd(&lock);
    EXPECT(lock == 1);
    //Try-acquire write lock with readers => failure
    EXPECT(p64_rwlock_try_acquire_wr(&lock) == false);
    p64_rwlock_release_rd(&lock);
    EXPECT(lock == 0);
    p64_rwlock_acquire_wr(&lock);
    EXPECT(lock == 0x80000000);
    //Try-acquire write lock with writer => failure
    EXPECT(p64_rwlock_try_acquire_wr(&lock) == false);
    //Try-acquire read lock with writer => failure
    EXPECT(p64_rwlock_try_acquire_rd(&lock) == false);
    p64_rwlock_release_wr(&lock);
    EXPECT(lock == 0);
    //Try-acquire write lock that is free => success
    EXPECT(p64_rwlock_try_acquire_wr(&lock) == true);
    p64_rwlock_release_wr(&lock);
    EXPECT(lock == 0);
    //Try-acquire read lock that is free => success
    EXPECT(p64_rwlock_try_acquire_rd(&lock) == true);
    EXPECT(lock == 1);
    p64_rwlock_release_rd(&lock);
    EXPECT(lock == 0);

    printf("rwlock tests complete\n");
    return 0;
}
