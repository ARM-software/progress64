//Copyright (c) 2024, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>

#include "p64_fiber.h"

#define STKSIZE 4096
#define NUM_FIBERS 4
#define OOPS_FIBER NUM_FIBERS

static void
filament(va_list *args)
{
    //Read my arguments
    int id = va_arg(*args, int);
    printf("fiber[%d]: created, va_list consumed\n", id);
    //Return to parent fiber
    p64_fiber_yield();
    //Fibers loop for different number of iterations
    for (int i = 0; i < id; i++)
    {
	printf("fiber[%d]: iteration %d\n", id, i);
	p64_fiber_yield();
    }
    if (id == 0)
    {
	printf("fiber[%d]: create additional fiber %d\n", id, OOPS_FIBER);
	char *stk = malloc(STKSIZE + sizeof(p64_fiber_t));
	p64_fiber_t *ctx = (void *)(stk + STKSIZE);
	if (stk == NULL)
	{
	    perror("malloc"), exit(EXIT_FAILURE);
	}
	p64_fiber_spawn(ctx, filament, stk, STKSIZE, OOPS_FIBER);
	//This stack will leak
    }
    printf("fiber[%d]: waiting at barrier\n", id);
    p64_fiber_barrier();
    printf("fiber[%d]: exit\n", id);
    //Call p64_fiber_exit() or just return from entrypoint function
    p64_fiber_exit();
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    char stack[NUM_FIBERS][STKSIZE];
    p64_fiber_t fibers[NUM_FIBERS];
//    p64_fiber_yield();//Bogus yield in main thread
    for (int i = 0; i < NUM_FIBERS; i++)
    {
	//Spawn a fiber, it will run immediately
	p64_fiber_spawn(&fibers[i], filament, stack[i], sizeof stack[i], i);
    }
    //Execute fibers, main thread blocks
    printf("main: letting fibers loose\n");
    p64_fiber_run();
    printf("main: all fibers have ceased\n");
    //All fibers exited
    p64_fiber_yield();//Bogus yield in main thread
    return 0;
}
