//Copyright (c) 2023, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause
#include <stdio.h>
#include "p64_hemlock.h"
#include "expect.h"
int main(void)
{
    p64_hemlock_t lock;
    p64_hemlock_init(&lock);
    p64_hemlock_acquire(&lock);
    p64_hemlock_release(&lock);
    p64_hemlock_acquire(&lock);
    p64_hemlock_release(&lock);
    printf("hemlock tests complete\n");
    return 0;
}
