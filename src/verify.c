//Copyright (c) 2024-2025, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "p64_coroutine.h"
#include "verify.h"
#include "build_config.h"

#define NUMCOROS 2
#define NUMSTEPS 64
#define STKSIZE (17 * 1024)
#define RET_DONE 0
#define RES_INIT 0
#define RES_EXEC 1
#define RES_FINI 2

uint32_t verify_id;//For use by datatype implementations

static volatile bool user_interrupt = false;
static bool VERBOSE = false;
static bool WARNERR = false;
static p64_coroutine_t CORO[NUMCOROS];
static _Alignas(64) char STACKS[NUMCOROS][STKSIZE];
#define INTERRUPTED NUMSTEPS
#define FAILED (NUMSTEPS + 1)
static uint64_t HISTO[NUMSTEPS + 2];

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

#include "atomic.h"

static void
ver_nop_exec(uint32_t id)
{
#if 1
    static _Alignas(CACHE_LINE) uint32_t head;
    static _Alignas(CACHE_LINE) uint32_t tail;
    if (id == 0)
    {
	//Produce (enqueue) at tail
	uint32_t t = atomic_load_n(&tail, __ATOMIC_RELAXED);
	uint32_t h = atomic_load_n(&head, __ATOMIC_ACQUIRE);//A0: Synchronize with A1
	(void)h;
	//Write ring[t & mask]
	atomic_store_n(&tail, t + 1, __ATOMIC_RELEASE);//B0: Synchronize with B1
    }
    else //id == 1
    {
	//Consume (dequeue) from head
	uint32_t h = atomic_load_n(&head, __ATOMIC_RELAXED);
	uint32_t t = atomic_load_n(&tail, __ATOMIC_ACQUIRE);//B1: Synchronize with B0
	(void)t;
	//Read ring[h & mask]
	atomic_store_n(&head, h + 1, __ATOMIC_RELEASE);//A1: Synchronize with A0
    }
#endif
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
extern struct ver_funcs ver_spinlock;
extern struct ver_funcs ver_blkring;
extern struct ver_funcs ver_hemlock;
extern struct ver_funcs ver_barrier;
extern struct ver_funcs ver_buckring1, ver_buckring2;
extern struct ver_funcs ver_cuckooht1, ver_cuckooht2;
extern struct ver_funcs ver_ringbuf_mpmc, ver_ringbuf_nbenbd, ver_ringbuf_nbelfd, ver_ringbuf_spsc;
extern struct ver_funcs ver_hopscotch1;
extern struct ver_funcs ver_linklist1, ver_linklist2, ver_linklist3, ver_linklist4;

//List of supported datatypes
static struct ver_funcs *ver_table[] =
{
    &ver_nop,
    &ver_lfstack,
    &ver_msqueue,
    &ver_clhlock,
    &ver_mcslock,
    &ver_spinlock,
    &ver_blkring,
    &ver_hemlock,
    &ver_barrier,
    &ver_buckring1, &ver_buckring2,
    &ver_cuckooht1, &ver_cuckooht2,
    &ver_ringbuf_mpmc, &ver_ringbuf_nbenbd, &ver_ringbuf_nbelfd, &ver_ringbuf_spsc,
    &ver_hopscotch1,
    &ver_linklist1, &ver_linklist2, &ver_linklist3, &ver_linklist4,
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

static const char *
rw_str(uint32_t fmt)
{
    switch (fmt & (V_READ | V_WRITE))
    {
	case 0       : return "--";
	case V_READ  : return "r-";
	case V_WRITE : return "-w";
	case V_RW    : return "rw";
	default      : return "??";
    }
}

static const char *
memo_str(int mo)
{
    switch (mo)
    {
	case V_REGULAR        : return "regular";
	case __ATOMIC_RELAXED : return "rlx";
	case __ATOMIC_ACQUIRE : return "acq";
	case __ATOMIC_RELEASE : return "rls";
	case __ATOMIC_ACQ_REL : return "acq_rls";
	case __ATOMIC_SEQ_CST : return "seq_cst";
	default : return "?";
    }
}

static bool
is_acq(int mo)
{
    return mo == __ATOMIC_ACQUIRE || mo == __ATOMIC_ACQ_REL || mo == __ATOMIC_SEQ_CST;
}

static bool
is_rls(int mo)
{
    return mo == __ATOMIC_RELEASE || mo == __ATOMIC_ACQ_REL || mo == __ATOMIC_SEQ_CST;
}

static void
print_result(const struct ver_file_line *fl, uint32_t id, uint32_t step, uintptr_t mask)
{
    printf("Step %2d: thread %u, ", step, id);
    if (fl != NULL && fl->file != NULL)
    {
	printf("file %s line %"PRIuPTR" ", fl->file, fl->line);
	if (fl->fmt & V_OP)
	{
	    uint32_t datasize = fl->fmt & 0xff;
	    assert(datasize <= 16);
	    printf("%s %s", rw_str(fl->fmt), fl->oper);
	    if (datasize != 0)
	    {
		printf("_%u", datasize);
	    }
	    printf("(");
	    if (fl->fmt & V_STR)
	    {
		printf("\"%s\"", (const char *)fl->addr);
	    }
	    else if (fl->fmt & V_AD)
	    {
		printf("%p", (void *)((uintptr_t)fl->addr & mask));
		if (datasize <= 8)
		{
		    if (fl->fmt & V_A1)
		    {
			printf(",%#"PRIxPTR, (intptr_t)(fl->arg1 & mask));
			if (fl->fmt & V_A2)
			{
			    printf(",%#"PRIxPTR, (intptr_t)(fl->arg2 & mask));
			}
		    }
		}
		else //datasize > 8
		{
		    if (fl->fmt & V_A1)
		    {
			printf(",%#"PRIxPTR"'%016"PRIxPTR,
				(uintptr_t)(fl->arg1 >> 64),
				(uintptr_t)fl->arg1);
			if (fl->fmt & V_A2)
			{
			    printf(",%#"PRIxPTR"'%016"PRIxPTR,
				    (uintptr_t)(fl->arg2 >> 64),
				    (uintptr_t)fl->arg2);
			}
		    }
		}
		printf(",%s", memo_str(fl->memo));
	    }
	    printf(")");
	    if (fl->fmt & V_RE)
	    {
		if (datasize <= 8)
		{
		    printf("=%#"PRIxPTR, (intptr_t)(fl->res & mask));
		}
		else //datasize > 8
		{
		    printf("=%#"PRIxPTR"'%016"PRIxPTR,
			    (uintptr_t)(fl->res >> 64),
			    (uintptr_t)fl->res);
		}
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

struct fileline
{
    const char *file0;
    uintptr_t line0;
    const char *file1;
    uintptr_t line1;
    uint64_t count;
};

static struct fileline syncs[257];
static struct fileline races[257];

static void
fileline_add(struct fileline *fl, const char *file0, uintptr_t line0, const char *file1, uintptr_t line1)
{
    uint32_t i = (line0 + 8192 * line1) % 257;
    if (fl[i].line0 == 0 && fl[i].line1 == 0)
    {
	fl[i].file0 = file0;
	fl[i].line0 = line0;
	fl[i].file1 = file1;
	fl[i].line1 = line1;
	fl[i].count = 1;
    }
    else
    {
	fl[i].count++;
    }
}

static void
print_syncs(void)
{
    for (uint32_t i = 0; i < 257; i++)
    {
	if (syncs[i].count != 0)
	{
	    printf("%s:%zu synchronizes-with %s:%zu (%lu)\n",
		    syncs[i].file0, syncs[i].line0,
		    syncs[i].file1, syncs[i].line1,
		    syncs[i].count);
	}
    }
}
static void
print_races(void)
{
    for (uint32_t i = 0; i < 257; i++)
    {
	if (races[i].count != 0)
	{
	    printf("%s:%zu data-races-with %s:%zu (%lu)\n",
		    races[i].file0, races[i].line0,
		    races[i].file1, races[i].line1,
		    races[i].count);
	}
    }
}

//Return true if [addr0:size0] is overlapping [addr1:size1]
static bool
overlap(uintptr_t addr0, uint32_t size0, uintptr_t addr1, uint32_t size1)
{
    //Check if before
    if (addr0 + size0 <= addr1)
    {
	return false;
    }
    //Check if after
    if (addr0 >= addr1 + size1)
    {
	return false;
    }
    //Some form of overlap
    return true;
}

enum verify_status { success, interrupted, failed };

struct trace
{
    struct ver_file_line fl;
    uint16_t id;
    int16_t syncw;//Synchronizes-with this step
};

static bool
analyze_memo(uint32_t id, uint32_t step, struct ver_file_line *fl, struct trace trace[])
{
    //Check for read operation
    if ((fl->fmt & V_READ) != 0)
    {
	//Try to find matching earlier write
	assert(fl->fmt & V_AD);
	uintptr_t addr = (uintptr_t)fl->addr;
	uint32_t size = fl->fmt & 0xff;
	for (int32_t i = step - 1; i >= 0; i--)
	{
	    uintptr_t addr_i = (uintptr_t)trace[i].fl.addr;
	    uint32_t size_i = trace[i].fl.fmt & 0xff;
	    if ((trace[i].fl.fmt & V_WRITE) && overlap(addr, size, addr_i, size_i))
	    {
		bool same = trace[i].id == id;
		if (VERBOSE)
		{
		    printf("%s read_%u on step %d matches %s write_%u from %s thread on step %d\n",
			    fl->memo == V_REGULAR ? "Regular" : "Atomic",
			    fl->fmt & 0xff,
			    step,
			    trace[i].fl.memo == V_REGULAR ? "regular" : "atomic",
			    trace[i].fl.fmt & 0xff,
			    same ? "same" : "other", i);
		}
		if (same)
		{
		    //Read matches write from same thread => ok
		    return true;
		}
		if (fl->memo != V_REGULAR && trace[i].fl.memo != V_REGULAR)
		{
		    //Atomic read matches atomic write from other thread => ok
		    if (is_acq(fl->memo) && is_rls(trace[i].fl.memo))
		    {
			//Load-acquire matches store-release => synchronizes-with
			trace[step].syncw = i;
			if (VERBOSE)
			{
			    printf("Step %d (%s:%zu) synchronizes-with step %d (%s:%zu)\n",
				    step, fl->file, fl->line,
				    i, trace[i].fl.file, trace[i].fl.line);
			}
			fileline_add(syncs, fl->file, fl->line, trace[i].fl.file, trace[i].fl.line);
		    }
		    else if (is_acq(fl->memo) && !is_rls(trace[i].fl.memo))
		    {
			if (VERBOSE)
			{
			    printf("Ignoring acquire-relaxed match\n");
			}
			continue;
		    }
		    return true;
		}
		//Else at least one memory access is regular (non-atomic), this could be a problem
		assert(fl->memo == V_REGULAR || trace[k].fl.memo == V_REGULAR);
		//Search for synchronize-with between steps-1..i+1
		for (int32_t j = step - 1; j > i; j--)
		{
		    if (trace[j].id == id && trace[j].syncw > i)
		    {
			if (VERBOSE)
			{
			    printf("Read on step %d matching write on step %d saved by synchronizes-with on steps %d-%d\n", step, i, j, trace[j].syncw);
			}
			return true;
		    }
		}
		if (VERBOSE)
		{
		    printf("ERROR: Read on step %d matching write on step %d missing synchronize-with!\n", step, i);
		}
		fileline_add(races, fl->file, fl->line, trace[i].fl.file, trace[i].fl.line);
		return false;
	    }
	    //Else no address match, continue search
	}
	//No matching write found
    }
    //Else not a read operation
    return true;
}

static void
verify(const struct ver_funcs *vf, uint64_t permutation, bool analyze, intptr_t mask)
{
    struct trace trace[NUMSTEPS + 1] = { 0 };
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
	trace[step].id = id;
	trace[step].syncw = -1;//Does not synchronize-with anything (yet)
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
	    trace[step].fl = *fl;//Save operation in trace
	    if (VERBOSE)
	    {
		print_result(fl, id, step, mask);
	    }
	    if (analyze)
	    {
		if (!analyze_memo(id, step, fl, trace))
		{
		    printf("Permutation %#lx step %u: Verification failed\n", permutation, step);
		    status = failed;
		    step++;//Ensure the last operation is saved in trace
		    break;
		}
	    }
	    //Check for yield to other thread
	    if (fl->fmt & V_YIELD)
	    {
		if (VERBOSE)
		{
		    printf("Yielding to other thread\n");
		}
		p &= ~(uint64_t)1;//Clear lsb
		p |= !id;//Set lsb to other id
	    }
	    //Else check for error or assertion failed, this aborts current verification
	    else if (fl->fmt & V_ABORT)
	    {
		printf("Permutation %#lx step %u: Verification failed\n", permutation, step);
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
	    print_result(&trace[i].fl, trace[i].id, i, mask);
	}
    }
}

static void
int_handler(int dummy)
{
    (void)dummy;
    if (user_interrupt)
    {
	//User already tried to interrupt execution
	static const char msg[] = "Forced interrupt\n";
	int r = write(1, msg, strlen(msg));
	(void)r;
	exit(EXIT_FAILURE);
    }
    user_interrupt = true;
}

static inline uint64_t
xorshift64(uint64_t x)
{
    //x == 0 will return 0
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return x;
}

int main(int argc, char *argv[])
{
#ifndef VERIFY
    fprintf(stderr, "Verification not enabled!\n"); fflush(stderr);
    exit(EXIT_FAILURE);
#endif
    (void)signal(SIGINT, int_handler);
    int64_t permutation = -1;//Steal one permutation
    uint64_t upper = (uint64_t)1 << 32;//Default upper bound to verify
    uintptr_t mask = ~(uintptr_t)0;
    uint64_t random = 0;
    bool analyze = false;
    int c;
    while ((c = getopt(argc, argv, "amp:r:u:vw")) != -1)
    {
	switch (c)
	{
	    case 'a' :
		analyze = true;
		break;
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
	    case 'r' :
		if (optarg[0] == '0' && optarg[1] == 'x')
		{
		    random = strtoul(optarg + 2, NULL, 16);
		}
		else if (optarg[0] == '0' && optarg[1] == 'b')
		{
		    random = strtoul(optarg + 2, NULL, 2);
		}
		else
		{
		    random = strtoul(optarg, NULL, 10);
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
	    case 'w' :
		WARNERR = true;
		break;
	    default :
usage:
		fprintf(stderr, "Usage: verify [<options>] <datatype>\n"
			"-a               Analyze memory orderings\n"
			"-m               Mask addresses and values to 32 bits when displaying\n"
			"-p <permutation> Specify permutation\n"
			"-r <seed>        Specify seed for random permutations\n"
			"-u <limit>       Specify upper limit of permutations to sweep\n"
			"-v               Verbose\n"
			"-w               Warnings become failures\n"
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
	verify(*vf, permutation, analyze, mask);
	print_syncs();
	print_races();
	return EXIT_SUCCESS;
    }
    else if (random != 0)
    {
	for (uint64_t iter = 0; iter < upper; iter++)
	{
	    if (!VERBOSE && iter % 100000 == 0)
	    {
		printf("Verifying permutation %#lx\n", random);
	    }
	    verify(*vf, random, analyze, mask);
	    if (user_interrupt)
	    {
		printf("Interrupted\n");
		break;
	    }
	    random = xorshift64(random);
	}
    }
    else
    {
	for (uint64_t perm = 0; perm < upper; perm++)
	{
	    if (!VERBOSE && perm % 100000 == 0)
	    {
		printf("Verifying permutation %#lx...\n", perm);
	    }
	    verify(*vf, perm, analyze, mask);
	    if (user_interrupt)
	    {
		printf("Interrupted\n");
		break;
	    }
	}
    }
    uint64_t succeeded = 0;
    for (uint32_t i = 0; i < NUMSTEPS; i++)
    {
	succeeded += HISTO[i];
	printf("%u: %lu\n", i, HISTO[i]);
    }
    printf("succeeded: %lu\n", succeeded);
    printf("interrupted: %lu\n", HISTO[INTERRUPTED]);
    printf("failed: %lu\n", HISTO[FAILED]);
    print_syncs();
    print_races();

    return EXIT_SUCCESS;
}
