//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>

#include "p64_hazardptr.h"
#include "p64_counter.h"
#include "expect.h"

#define NUM_HAZARD_POINTERS 1
#define NUM_COUNTERS 2

int main(void)
{
    printf("testing counter\n");

    p64_hpdomain_t *hpd;
    p64_cntdomain_t *cntd;
    p64_counter_t cntid, cntid2;

    hpd = p64_hazptr_alloc(10, NUM_HAZARD_POINTERS);
    EXPECT(hpd != NULL);
    p64_hazptr_register(hpd);

    cntd = p64_cntdomain_alloc(NUM_COUNTERS, P64_COUNTER_F_HP);
    EXPECT(cntd != NULL);

    cntid = p64_counter_alloc(cntd);
    EXPECT(cntid != P64_COUNTER_INVALID);
    EXPECT(p64_counter_read(cntd, cntid) == 0);

    //Register thread as a client in order to do updates
    p64_cntdomain_register(cntd);

    p64_counter_add(cntd, cntid, 242);
    EXPECT(p64_counter_read(cntd, cntid) == 242);
    p64_counter_add(cntd, cntid, 20);
    EXPECT(p64_counter_read(cntd, cntid) == 262);

    cntid2 = p64_counter_alloc(cntd);
    EXPECT(cntid2 != P64_COUNTER_INVALID);

    EXPECT(p64_counter_alloc(cntd) == P64_COUNTER_INVALID);

    //Reset while registered
    p64_counter_reset(cntd, cntid);
    EXPECT(p64_counter_read(cntd, cntid) == 0);
    p64_counter_add(cntd, cntid, 42);
    EXPECT(p64_counter_read(cntd, cntid) == 42);

    //Unregister thread as a client
    p64_cntdomain_unregister(cntd);
    //Verify counter value can still be read
    EXPECT(p64_counter_read(cntd, cntid) == 42);

    //Reset when not registered
    p64_counter_reset(cntd, cntid);
    EXPECT(p64_counter_read(cntd, cntid) == 0);

    //Free all counters
    p64_counter_free(cntd, cntid);
    p64_counter_free(cntd, cntid2);

    //Reallocate one of the freed counters
    cntid = p64_counter_alloc(cntd);
    EXPECT(cntid != P64_COUNTER_INVALID);
    p64_counter_free(cntd, cntid);

    //Ensure any retired objects have actually been reclaimed
    while (p64_hazptr_reclaim() != 0)
    {
	(void)0;
    }
    p64_hazptr_unregister();

    p64_cntdomain_free(cntd);
    p64_hazptr_free(hpd);

    printf("counter test complete\n");
    return 0;
}
