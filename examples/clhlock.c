//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdio.h>
#include "p64_clhlock.h"
#include "expect.h"

int main(void)
{
    p64_clhlock_t lock;
    p64_clhnode_t *node = NULL;
    p64_clhlock_init(&lock);
    p64_clhlock_acquire(&lock, &node);
    p64_clhlock_release(&node);
    p64_clhlock_acquire(&lock, &node);
    p64_clhlock_release(&node);
    p64_clhlock_fini(&lock);

    printf("clhlock tests complete\n");
    return 0;
}
