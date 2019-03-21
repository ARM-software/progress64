//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "p64_qsbr.h"
#include "expect.h"

static const char *expect = NULL;

void
callback(void *ptr)
{
    EXPECT(expect != NULL);
    printf("Reclaiming %s\n", (const char *)ptr);
    EXPECT(strcmp(ptr, expect) == 0);
}

int main(void)
{
    bool b;
    uint32_t r;
    p64_qsbr_t *qsbr = p64_qsbr_alloc(10);
    EXPECT(qsbr != NULL)
    p64_qsbr_register(qsbr);
    p64_qsbr_acquire();
    b = p64_qsbr_retire("X", callback);
    EXPECT(b == true);
    r = p64_qsbr_reclaim();
    EXPECT(r == 1);//1 unreclaimed object
    //Thread reports no saved references, X can now be reclaimed
    p64_qsbr_quiescent();
    p64_qsbr_quiescent();
    b = p64_qsbr_retire("Y", callback);
    EXPECT(b == true);
    expect = "X";
    r = p64_qsbr_reclaim();
    EXPECT(r == 1);//1 unreclaimed object (Y)
    expect = NULL;
    //Thread reports no saved references, Y can now be reclaimed
    p64_qsbr_quiescent();
    expect = "Y";
    r = p64_qsbr_reclaim();
    EXPECT(r == 0);//0 unreclaimed objects
    expect = NULL;
    p64_qsbr_unregister();
    p64_qsbr_free(qsbr);

    printf("qsbr tests complete\n");
    return 0;
}
