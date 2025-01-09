//Copyright (c) 2024-2025, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "p64_coroutine.h"
#include "verify.h"

#define NUMCOROS 2
#define NUMSTEPS 64
#define STKSIZE (17 * 1024)
#define RET_DONE 0
#define RES_INIT 0
#define RES_EXEC 1
#define RES_FINI 2

uint32_t verify_id;//For use by datatype implementations

static bool VERBOSE = false;
static p64_coroutine_t CORO[NUMCOROS];
static _Alignas(64) char STACKS[NUMCOROS][STKSIZE];
#define INTERRUPTED NUMSTEPS
#define FAILED (NUMSTEPS + 1)
static uint32_t HISTO[NUMSTEPS + 2];

static void
ver_nop_init(uint32_t numthreads)
{
    (void)numthreads;
}

static void
ver_nop_fini(uint32_t numthreads)
{
    (void)numthreads;
}

static void
ver_nop_exec(uint32_t id)
{
    (void)id;
#if 0
    for (;;)
    {
	//VERIFY_SUSPEND(V_OP, "nop", NULL, 0, 0, 0);
	VERIFY_YIELD();
    }
#endif
}

static struct ver_funcs ver_nop =
{
    "nop", ver_nop_init, ver_nop_exec, ver_nop_fini
};

extern struct ver_funcs ver_lfstack;
extern struct ver_funcs ver_msqueue;
extern struct ver_funcs ver_clhlock;
extern struct ver_funcs ver_mcslock;
extern struct ver_funcs ver_blkring;
extern struct ver_funcs ver_hemlock;
extern struct ver_funcs ver_barrier;
extern struct ver_funcs ver_buckring1, ver_buckring2;
extern struct ver_funcs ver_cuckooht1, ver_cuckooht2;

//List of supported datatypes
static struct ver_funcs *ver_table[] =
{
    &ver_nop,
    &ver_lfstack,
    &ver_msqueue,
    &ver_clhlock,
    &ver_mcslock,
    &ver_blkring,
    &ver_hemlock,
    &ver_barrier,
    &ver_buckring1, &ver_buckring2,
    &ver_cuckooht1, &ver_cuckooht2,
    NULL
};

//Coroutine main function
//Will invoke the specified datatype
static intptr_t
coroutine(va_list *args)
{
    //Spawn phase, read arguments
    const struct ver_funcs *vf = va_arg(*args, struct ver_funcs *);
    int id = va_arg(*args, int);
    if (id == 0)
    {
	vf->init(NUMCOROS);
    }
    //Initialization complete, suspend
    (void)p64_coro_suspend(0);//Parameter is returned from spawn() call
    //Execution started
    vf->exec(id);
    //Execution complete, return RET_DONE
    for (;;)
    {
	intptr_t r = p64_coro_suspend((intptr_t)RET_DONE);
	if (r == RES_FINI)
	{
	    vf->fini(NUMCOROS);
	}
    }
    return 0;
}

static void
print_result(const struct ver_file_line *fl, uint32_t id, uint32_t step, uintptr_t mask)
{
    printf("Step %d: thread %u, ", step, id);
    if (fl != NULL && fl->file != NULL)
    {
	printf("file %s line %"PRIuPTR" ", fl->file, fl->line);
	if (fl->fmt & V_OP)
	{
	    printf("%s(", fl->oper);
	    if (fl->fmt & V_STR)
	    {
		printf("\"%s\"", (const char *)fl->addr);
	    }
	    else if (fl->fmt & V_AD)
	    {
		printf("%p", (void *)((uintptr_t)fl->addr & mask));
		if (fl->fmt & V_A1)
		{
		    printf(",%#"PRIxPTR, (fl->arg1 & mask));
		    if (fl->fmt & V_A2)
		    {
			printf(",%#"PRIxPTR, (fl->arg2 & mask));
		    }
		}
	    }
	    printf(")");
	    if (fl->fmt & V_RE)
	    {
		printf("=%#"PRIxPTR, (fl->res & mask));
	    }
	}
	printf("\n");
    }
    else
    {
	printf("done\n");
    }
}

static bool
exec_coroutine(p64_coroutine_t *cr,
	       intptr_t arg,
	       intptr_t r,
	       intptr_t mask)
{
    for (;;)
    {
	r = p64_coro_resume(cr, arg);
	if (r == 0)
	{
	    return true;
	}
	struct ver_file_line *fl = (struct ver_file_line *)r;
	if (VERBOSE || (fl->fmt & V_ABORT) != 0)
	{
	    print_result(fl, 0, arg == RES_INIT ? -1 : NUMSTEPS, mask);
	}
	if ((fl->fmt & V_ABORT) != 0)
	{
	    return false;
	}
	r = 0;
    }
}

enum verify_status { success, interrupted, failed };

static void
verify(const struct ver_funcs *vf, uint64_t permutation, intptr_t mask)
{
    struct ver_file_line trace[NUMSTEPS + 1] = { 0 };
    uint32_t trace_id[NUMSTEPS + 1] = { 0 };
    enum verify_status status = interrupted;
    intptr_t res[NUMCOROS] = { !RET_DONE, !RET_DONE };
    uint64_t p = permutation;
    uint32_t step = 0;
    if (VERBOSE)
    {
	printf("Verifying %s using permutation %#lx\n", vf->name, permutation);
    }
    //Spawn all coroutines
    for (uint32_t id = 0; id < NUMCOROS; id++)
    {
	verify_id = id;
	intptr_t r = p64_coro_spawn(&CORO[id], coroutine, STACKS[id], sizeof STACKS[id], vf, id);
	if (r != 0)
	{
	    struct ver_file_line *fl = (struct ver_file_line *)r;
	    if (VERBOSE)
	    {
		print_result(fl, 0, -1, mask);
	    }
	    if (!exec_coroutine(&CORO[id], RES_INIT, r, mask))
	    {
		printf("Verification of permutation %#lx failed at init\n", permutation);
		status = failed;
		goto failure;
	    }
	}
    }
    //Execute coroutines according to the permutation
    for (step = 0; step < NUMSTEPS; step++)
    {
	uint32_t id = p & 1;//Compute which coroutine (thread) to execute next
	verify_id = id;//Assign to global variable as well for external use
	//Resume identified coroutine
	intptr_t ret = p64_coro_resume(&CORO[id], RES_EXEC);
	trace_id[step] = id;
	//RET_DONE return means coroutine completed
	if (ret == RET_DONE)
	{
	    res[id] = RET_DONE;
	    if (VERBOSE)
	    {
		print_result(NULL, id, step, mask);
	    }
	    //One thread done, check the other
	    if (res[!id] == RET_DONE)
	    {
		//Other thread also done
		step++;//Ensure DONE marker also saved in trace
		status = success;
		break;//Exit loop
	    }
	    //Else other thread still incomplete
	    //Only run the other thread from now
	    p = (id == 0) ? ~(uint64_t)0 : 0;
	}
	else
	{
	    struct ver_file_line *fl = (struct ver_file_line *)ret;
	    res[id] = !RET_DONE;
	    trace[step] = *fl;//Save operation in trace
	    if (VERBOSE)
	    {
		print_result(fl, id, step, mask);
	    }
	    //Check for 'force' yield to other thread
	    if (fl->fmt & V_FORCE)
	    {
		if (VERBOSE)
		{
		    printf("Forcing other thread to run\n");
		}
		p &= ~(uint64_t)1;//Clear lsb
		p |= !id;//Set lsb to other id
	    }
	    //Else check for error or assertion failed, this aborts current verification
	    else if ((fl->fmt & V_ABORT) != 0)
	    {
		printf("Verification of permutation %#lx failed at step %u\n", permutation, step);
		status = failed;
		step++;//Ensure the error/assert info is saved in trace
		break;
	    }
	    else
	    {
		//Remove last used id
		p >>= 1;
	    }
	}
    }
    if (status == success)
    {
	//Resume completed coroutine for it to execute the fini function
	if (!exec_coroutine(&CORO[0], RES_FINI, 0, mask))
	{
	    //Some failure or error reported
	    printf("Verification of permutation %#lx failed at fini\n", permutation);
	    status = failed;
	    step++;//Ensure the error info is saved in trace
	    goto failure;
	}
	else if (VERBOSE)
	{
	    printf("Verification of permutation %#lx complete after %u steps\n", permutation, step);
	}
	assert(step < NUMSTEPS);
	HISTO[step]++;
    }
    //Else failure/abort, can't call fini() function
    else
    {
failure:
	if (status == interrupted)
	{
	    printf("Verification of permutation %#lx interrupted after %u steps\n", permutation, step);
	    status = interrupted;
	    HISTO[INTERRUPTED]++;
	}
	else//status == failed
	{
	    HISTO[FAILED]++;
	}
	//Print the steps that led to here
	for (uint32_t i = 0; i < step; i++)
	{
	    print_result(&trace[i], trace_id[i], i, mask);
	}
    }
}

int main(int argc, char *argv[])
{
#ifndef VERIFY
    fprintf(stderr, "Verification not enabled!\n"); fflush(stderr);
    exit(EXIT_FAILURE);
#endif
    int64_t permutation = -1;//Steal one permutation
    uint64_t upper = (uint64_t)1 << 32;//Default upper bound to verify
    uintptr_t mask = ~(uintptr_t)0;
    int c;
    while ((c = getopt(argc, argv, "mp:u:v")) != -1)
    {
	switch (c)
	{
	    case 'm' :
		mask = (uint32_t)mask;
		break;
	    case 'p' :
		if (optarg[0] == '0' && optarg[1] == 'x')
		{
		    permutation = strtoul(optarg + 2, NULL, 16);
		}
		else if (optarg[0] == '0' && optarg[1] == 'b')
		{
		    permutation = strtoul(optarg + 2, NULL, 2);
		}
		else
		{
		    permutation = strtoul(optarg, NULL, 10);
		}
		break;
	    case 'u' :
		if (optarg[0] == '0' && optarg[1] == 'x')
		{
		    upper = strtoul(optarg + 2, NULL, 16);
		}
		else if (optarg[0] == '0' && optarg[1] == 'b')
		{
		    upper = strtoul(optarg + 2, NULL, 2);
		}
		else
		{
		    upper = strtoul(optarg, NULL, 10);
		}
		break;
	    case 'v' :
		VERBOSE = true;
		break;
	    default :
usage:
		fprintf(stderr, "Usage: verify [<options>] <datatype>\n"
			"-m               Mask addresses and values to 32 bits when displaying\n"
			"-p <permutation> Specify permutation\n"
			"-u <limit>       Specify upper limit of permutations to sweep\n"
			"-v               Verbose\n"
		       );
list_datatypes:
		fprintf(stderr, "Known datatypes:\n");
		for (struct ver_funcs **vf = ver_table; *vf != NULL; vf++)
		{
		    fprintf(stderr, "%s\n", (*vf)->name);
		}
		exit(EXIT_FAILURE);
	}
    }
    if (optind != argc - 1)
    {
	goto usage;
    }
    struct ver_funcs **vf;
    for (vf = ver_table; *vf != NULL; vf++)
    {
	if (strcmp((*vf)->name, argv[optind]) == 0)
	{
	    break;
	}
    }
    if (*vf == NULL)
    {
	fprintf(stderr, "Unknown datatype %s specified\n", argv[optind]);
	goto list_datatypes;
    }

    if (!VERBOSE)
    {
	printf("Verifying %s\n", (*vf)->name);
    }
    if (permutation != -1)
    {
	verify(*vf, permutation, mask);
    }
    else
    {
	for (uint64_t perm = 0; perm < upper; perm++)
	{
	    if (!VERBOSE && perm % 100000 == 0)
	    {
		printf("Verifying permutation %#lx...\n", perm);
	    }
	    verify(*vf, perm, mask);
	}
	for (uint32_t i = 0; i < NUMSTEPS; i++)
	{
	    printf("%u: %u\n", i, HISTO[i]);
	}
	printf("interrupted: %u\n", HISTO[INTERRUPTED]);
	printf("failed: %u\n", HISTO[FAILED]);
    }

    return EXIT_SUCCESS;
}
