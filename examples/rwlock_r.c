//Copyright (c) 2018-2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdio.h>
#include <string.h>
#include "p64_rwlock_r.h"
#include "expect.h"

int main(void)
{
    p64_rwlock_r_t lock_r;
    p64_rwlock_r_init(&lock_r);

    p64_rwlock_r_acquire_rd(&lock_r);
    p64_rwlock_r_acquire_rd(&lock_r);
    p64_rwlock_r_release_rd(&lock_r);
    p64_rwlock_r_release_rd(&lock_r);

    p64_rwlock_r_acquire_wr(&lock_r);
    p64_rwlock_r_acquire_wr(&lock_r);
    p64_rwlock_r_release_wr(&lock_r);
    p64_rwlock_r_release_wr(&lock_r);

    p64_rwlock_r_acquire_rd(&lock_r);
    p64_rwlock_r_release_rd(&lock_r);

    printf("rwlock_r tests complete\n");
    return 0;
}
