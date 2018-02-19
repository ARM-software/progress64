//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "p64_antireplay.h"
#include "expect.h"

int main(void)
{
    p64_antireplay_t *ar = p64_antireplay_alloc(256, false);
    EXPECT(ar != NULL);
    EXPECT(p64_antireplay_test_and_set(ar, 100) == p64_ar_pass);
    EXPECT(p64_antireplay_test_and_set(ar, 100) == p64_ar_replay);
    EXPECT(p64_antireplay_test(ar, 356) == p64_ar_pass);
    EXPECT(p64_antireplay_test_and_set(ar, 356) == p64_ar_pass);
    EXPECT(p64_antireplay_test(ar, 100) == p64_ar_stale);
    EXPECT(p64_antireplay_test_and_set(ar, 100) == p64_ar_stale);
    EXPECT(p64_antireplay_test_and_set(ar, 356) == p64_ar_replay);
    p64_antireplay_free(ar);

    printf("antireplay tests complete\n");
    return 0;
}
