//Copyright (c) 2024, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "p64_fiber.h"

#define LIKELY(x)    __builtin_expect(!!(x), 1)
#define UNLIKELY(x)  __builtin_expect(!!(x), 0)

#define MAX_FIBERS 10
#define STKSIZE 4096

static bool VERBOSE;
static uint32_t COUNTER;
static uint32_t NUMYIELDS;
static p64_fiber_t FIBERS[MAX_FIBERS];
static char STACKS[MAX_FIBERS][STKSIZE];

static void
fiber(va_list *args)
{
    int id = va_arg(*args, int);
    if (VERBOSE)
    {
	printf("fiber[%d] spawned\n", id);
    }
    p64_fiber_yield();
    while (LIKELY(COUNTER++ < NUMYIELDS))
    {
	p64_fiber_yield();
    }
    p64_fiber_exit();
}

static void
benchmark(uint32_t numyields, uint32_t numfibers, uint32_t cpufreq)
{
    struct timespec ts;
    for (uint32_t i = 0; i < numfibers; i++)
    {
	p64_fiber_spawn(&FIBERS[i], fiber, STACKS[i], sizeof STACKS[i], i);
    }
    COUNTER = 0;
    NUMYIELDS = numyields;

    //Read starting time
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t start = ts.tv_sec * 1000000000ULL + ts.tv_nsec;
    p64_fiber_run();
    //Read end time
    clock_gettime(CLOCK_MONOTONIC, &ts);

    uint64_t elapsed_ns = ts.tv_sec * 1000000000ULL + ts.tv_nsec - start;
    uint32_t elapsed_s = elapsed_ns / 1000000000ULL;
    uint32_t upd_per_sec = 0;
    if (elapsed_ns != 0)
    {
	upd_per_sec = (uint32_t)(1000000000ULL * numyields / elapsed_ns);
	printf("%9u yields/s", upd_per_sec);
    }
    else
    {
	printf("INF yields/s");
    }
    printf(", %u.%04llu secs, yields %u, ",
	    elapsed_s,
	    (elapsed_ns % 1000000000LLU) / 100000LLU,
	    numyields);
    if (cpufreq != 0)
    {
	uint64_t cycles = elapsed_ns * cpufreq / 1000000ULL;
	printf("%2u cycles/update, ", (uint32_t)(cycles / numyields));
    }
    printf("nfibers %u\n", numfibers);
}

int main(int argc, char *argv[])
{
    uint32_t cpufreq = 0;
    uint32_t numyields = 100000000;
    uint32_t numfibers = 2;
    int c;

    while ((c = getopt(argc, argv, "f:n:vy:")) != -1)
    {
	switch (c)
	{
	    case 'f' :
		{
		    int f = atoi(optarg);
		    if (f < 0)
		    {
			fprintf(stderr, "Invalid frequency %d\n", f);
			exit(EXIT_FAILURE);
		    }
		    cpufreq = f;
		    break;
		}
	    case 'n' :
		{
		    int ni = atoi(optarg);
		    if (ni < 1)
		    {
			fprintf(stderr, "Invalid number of fibers %d\n", ni);
			exit(EXIT_FAILURE);
		    }
		    numfibers = (unsigned)ni;
		    break;
		}
	    case 'v' :
		VERBOSE = true;
		break;
	    case 'y' :
		{
		    int ni = atoi(optarg);
		    if (ni < 1)
		    {
			fprintf(stderr, "Invalid number of yields %d\n", ni);
			exit(EXIT_FAILURE);
		    }
		    numyields = (unsigned)ni;
		    break;
		}
	    default :
usage :
		fprintf(stderr, "Usage: bm_atomic_lat <options>\n"
			"-f <cpufreq>     CPU frequency in KHz\n"
			"-n <numfibers>   Number of fibers\n"
			"-v               Verbose\n"
			"-y <numyields>   Number of yields\n"
		       );
		exit(EXIT_FAILURE);
	}
    }
    if (optind != argc)
    {
	goto usage;
    }
    benchmark(numyields, numfibers, cpufreq);
    return 0;
}
