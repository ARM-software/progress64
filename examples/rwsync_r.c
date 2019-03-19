//Copyright (c) 2018-2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdio.h>
#include <string.h>
#include "p64_rwsync_r.h"
#include "expect.h"


int main(void)
{
    p64_rwsync_r_t sync_r;
    p64_rwsync_t s, ss;
    char data[37] = { [36] = 255 };
    p64_rwsync_r_init(&sync_r);
    s = p64_rwsync_r_acquire_rd(&sync_r);
    EXPECT(p64_rwsync_r_release_rd(&sync_r, s) == true);
    //First acquire-read call
    s = p64_rwsync_r_acquire_rd(&sync_r);
    //Second (recursive) acquire-read call
    ss = p64_rwsync_r_acquire_rd(&sync_r);
    //First acquire-write call
    p64_rwsync_r_acquire_wr(&sync_r);
    //Second (recursive) acquire-write call
    p64_rwsync_r_acquire_wr(&sync_r);
    //Write the protected data
    strcpy(data, "Daisy, Daisy give me your answer do");
    //Second (recursive) release-read call
    EXPECT(p64_rwsync_r_release_rd(&sync_r, ss) == false);
    //Second (recursive) release-write call
    p64_rwsync_r_release_wr(&sync_r);
    //First release-read call
    EXPECT(p64_rwsync_r_release_rd(&sync_r, s) == false);
    //First release-write call
    p64_rwsync_r_release_wr(&sync_r);
    EXPECT(memcmp(data, "Daisy, Daisy give me your answer do", 36) == 0);
    EXPECT(data[36] == (char)255);

    printf("rwsync_r tests complete\n");
    return 0;
}
