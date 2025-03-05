//Copyright (c) 2025, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdio.h>
#include "p64_rplock.h"
#include "expect.h"

int main(void)
{
    bool success;
    p64_rpnode_t node, node2;
    p64_rplock_t lock;
    p64_rplock_init(&lock);
    p64_rplock_acquire(&lock, &node);
    p64_rplock_release(&lock, &node);
    success = p64_rplock_try_acquire(&lock, &node);
    EXPECT(success == true);
    success = p64_rplock_try_acquire(&lock, &node2);
    EXPECT(success == false);
    p64_rplock_release(&lock, &node);
    p64_rplock_acquire(&lock, &node2);
    p64_rplock_release(&lock, &node2);
    printf("rplock tests complete\n");
    return 0;
}
