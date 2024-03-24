//Copyright (c) 2024, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>

#include "p64_coroutine.h"

static intptr_t
echo(va_list *args)
{
    //Read my arguments
    uint32_t val = va_arg(*args, uint32_t);
    printf("echo: spawned with arg %u\n", val);
    //Return to p64_coro_spawn call after we have acquired arguments
    intptr_t arg = p64_coro_suspend(val);
    printf("echo: resumed with arg %zu\n", arg);
    p64_coro_suspend(arg);
    printf("echo: resumed with arg %zu\n", arg);
    p64_coro_return(arg);
}

//Parameters passed between parent and coroutine
#define GEN_ARGS_OK 0
#define GEN_CONTINUE 1
#define GEN_END 2

static intptr_t
generator(va_list *args)
{
    //Read my arguments
    uint32_t bgn = va_arg(*args, uint32_t);
    uint32_t end = va_arg(*args, uint32_t);
    uint32_t *ptr = va_arg(*args, uint32_t *);
    printf("generator: bgn %u, end %u, ptr %p\n", bgn, end, ptr);
    //Return to p64_coro_spawn call after we have acquired arguments
    intptr_t arg = p64_coro_suspend(GEN_ARGS_OK);
    //Parent resumed us for the first time
    printf("generator: p64_coro_suspend() returned %zd\n", arg);
    for (uint32_t n = bgn; n < end; n++)
    {
	*ptr = n;
	arg = p64_coro_suspend(GEN_CONTINUE);
	printf("generator: p64_coro_suspend() returned %zd\n", arg);
    }
    printf("generator: spawning echo coroutine\n");
    char stack[4096];//Allocated on the stack of generator coroutine
    p64_coroutine_t coro_echo;
    arg = p64_coro_spawn(&coro_echo, echo, stack, sizeof stack, 242);
    printf("generator: p64_coro_spawn() returned %zu (expected %u)\n", arg, 242);
    //Lateral call to echo coroutine
    arg = p64_coro_switch(&coro_echo, 256);
    printf("generator: p64_coro_switch() returned %zu\n", arg);
    arg = p64_coro_resume(&coro_echo, 262);
    printf("generator: p64_coro_resume() returned %zu (expected %u)\n", arg, 262);
    //Returning GEN_END indicates end of number generation
    return GEN_END;
    //We could also call p64_coro_return(GEN_END);
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    char stack[8192];
    p64_coroutine_t coro_gen;
    uint32_t number;
    //Spawn a coroutine, it will run immediately
    //Be careful to match types used in va_args calls in generator function
    intptr_t arg = p64_coro_spawn(&coro_gen, generator, stack, sizeof stack, 100, 110, &number);
    if (arg != GEN_ARGS_OK)
    {
	fprintf(stderr, "main: p64_coro_spawn() returned %zd, expected %u\n", arg, GEN_ARGS_OK);
	fflush(stderr);
	abort();
    }
    printf("main: p64_coro_spawn() returned %zd\n", arg);
    //Resume the coroutine to generate the next number
    while ((arg = p64_coro_resume(&coro_gen, GEN_CONTINUE)) != GEN_END)
    {
	printf("main: p64_coro_resume() returned %zd\n", arg);
	printf("number: %u\n", number);
    }
    //Coroutine has returned
    printf("main: p64_coro_resume() returned %zd\n", arg);

    return 0;
}
