//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdio.h>
#include "p64_rwsync.h"
#include "expect.h"

int main(void)
{
    p64_rwsync_t sync;
    p64_rwsync_t s;
    char data[23];
    p64_rwsync_init(&sync);
    s = p64_rwsync_acquire_rd(&sync);
    EXPECT(p64_rwsync_release_rd(&sync, s) == true);
    s = p64_rwsync_acquire_rd(&sync);
    p64_rwsync_write(&sync, "Mary had a little lamb", data, 23);
    EXPECT(p64_rwsync_release_rd(&sync, s) == false);

    printf("rwsync tests complete\n");
    return 0;
}
