//Copyright (c) 2020, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdio.h>
#include "p64_mcslock.h"

int main(void)
{
    p64_mcslock_t lock;
    p64_mcsnode_t node;
    p64_mcslock_init(&lock);
    p64_mcslock_acquire(&lock, &node);
    p64_mcslock_release(&lock, &node);
    p64_mcslock_acquire(&lock, &node);
    p64_mcslock_release(&lock, &node);

    printf("mcslock tests complete\n");
    return 0;
}
