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
#include "atomic.h"

//Number of coroutines (threads)
#define NUMCOROS 2
//Number of steps before thread (coroutine) execution is interrupted
#define NUMSTEPS 96
//Coroutine stack size, use prime factor to decrease risk of aliasing between coroutines
#define STKSIZE (17 * 1024)
//Coroutine return value
#define RET_DONE 0
//Coroutine commands
#define RES_INIT 0
#define RES_EXEC 1
#define RES_FINI 2

//For use by verification module implementations that use TLS
//As all coroutines execute in the same host thread, they will also share TLS
//We need to complement TLS with a thread identifer of the currently running coroutine
uint32_t verify_id;

//User attempted to interrupt exeuction (e.g. CTRL-C)
static volatile bool user_interrupt = false;
//CLI flags
static bool VERBOSE = false;
static bool WARNERR = false;
//Our coroutines and their stacks
static p64_coroutine_t CORO[NUMCOROS];
static _Alignas(64) char STACKS[NUMCOROS][STKSIZE];
//Special purpose indexes into HISTO array
#define INTERRUPTED NUMSTEPS
#define FAILED (NUMSTEPS + 1)
static uint64_t HISTO[NUMSTEPS + 2];

//A test module called "test"
//It implements a simple SPSC ring buffer
static void
ver_test_init(uint32_t numthreads)
{
    (void)numthreads;
}

static void
ver_test_fini(uint32_t numthreads)
{
    (void)numthreads;
}

static void
ver_test_exec(uint32_t id)
{
    static _Alignas(CACHE_LINE) uint32_t head;
    static _Alignas(CACHE_LINE) uint32_t tail;
    static uint32_t ring[16];
    static const uint32_t mask = 15;
    if (id == 0)
    {
	//Produce (enqueue) at tail
	uint32_t t = atomic_load_n(&tail, __ATOMIC_RELAXED);
	uint32_t h = atomic_load_n(&head, __ATOMIC_ACQUIRE);//A0: Synchronize with A1
	(void)h;
	regular_store_n(&ring[t & mask], 242);
	atomic_store_n(&tail, t + 1, __ATOMIC_RELEASE);//B0: Synchronize with B1
    }
    else //id == 1
    {
	//Consume (dequeue) from head
	uint32_t h = atomic_load_n(&head, __ATOMIC_RELAXED);
	uint32_t t = atomic_load_n(&tail, __ATOMIC_ACQUIRE);//B1: Synchronize with B0
	if (t - head > 0)
	{
	    VERIFY_ASSERT(regular_load_n(&ring[h & mask]) == 242);
	    atomic_store_n(&head, h + 1, __ATOMIC_RELEASE);//A1: Synchronize with A0
	}
    }
}

static struct ver_funcs ver_test =
{
    "test", ver_test_init, ver_test_exec, ver_test_fini
};

extern struct ver_funcs ver_lfstack;
extern struct ver_funcs ver_msqueue;
extern struct ver_funcs ver_mcqueue;
extern struct ver_funcs ver_deque1, ver_deque2, ver_deque3;
extern struct ver_funcs ver_clhlock;
extern struct ver_funcs ver_mcslock;
extern struct ver_funcs ver_spinlock;
extern struct ver_funcs ver_blkring;
extern struct ver_funcs ver_hemlock;
extern struct ver_funcs ver_rplock;
extern struct ver_funcs ver_barrier;
extern struct ver_funcs ver_buckring1, ver_buckring2;
extern struct ver_funcs ver_cuckooht1, ver_cuckooht2;
extern struct ver_funcs ver_ringbuf_mpmc, ver_ringbuf_nbenbd, ver_ringbuf_nbelfd, ver_ringbuf_spsc;
extern struct ver_funcs ver_hopscotch1;
extern struct ver_funcs ver_linklist1, ver_linklist2, ver_linklist3, ver_linklist4;

//Table of registered verification modules
static struct ver_funcs *ver_table[] =
{
    &ver_test,
    &ver_lfstack,
    &ver_msqueue,
    &ver_mcqueue,
    &ver_deque1, &ver_deque2, &ver_deque3,
    &ver_clhlock,
    &ver_mcslock,
    &ver_spinlock,
    &ver_blkring,
    &ver_hemlock,
    &ver_rplock,
    &ver_barrier,
    &ver_buckring1, &ver_buckring2,
    &ver_cuckooht1, &ver_cuckooht2,
    &ver_ringbuf_mpmc, &ver_ringbuf_nbenbd, &ver_ringbuf_nbelfd, &ver_ringbuf_spsc,
    &ver_hopscotch1,
    &ver_linklist1, &ver_linklist2, &ver_linklist3, &ver_linklist4,
    NULL
};

//Coroutine main function
//It will invoke the specified verification module
static intptr_t
coroutine(va_list *args)
{
    //Spawn phase, read arguments (must match p64_coro_spawn() arguments)
    const struct ver_funcs *vf = va_arg(*args, struct ver_funcs *);
    int id = va_arg(*args, int);
    if (id == 0)
    {
	//Only thread 0 executes the init function
	vf->init(NUMCOROS);
    }
    //Initialization complete, suspend
    (void)p64_coro_suspend(0);//Parameter 0 is returned from spawn call
    //Execution started
    vf->exec(id);
    //Execution complete, return RET_DONE
    for (;;)
    {
	intptr_t r = p64_coro_suspend((intptr_t)RET_DONE);
	if (id == 0 && r == RES_FINI)
	{
	    //Only thread 0 executes the fini function
	    vf->fini(NUMCOROS);
	}
    }
    return 0;
}

//Return string representing read/write operation returned by thread
static const char *
rw_str(uint32_t fmt)
{
    switch (fmt & (V_READ | V_WRITE))
    {
	case 0       : return "--";
	case V_READ  : return "r-";
	case V_WRITE : return "-w";
	case V_RW    : return "rw";
	default      : return "??";//Needed to silence compiler
    }
}

//Return string reprsenting memory ordering by operation returned by thread
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
	default               : return "?";
    }
}

//Does the memory ordering represent an acquire operation?
static bool
is_acq(int mo)
{
    return mo == __ATOMIC_ACQUIRE || mo == __ATOMIC_ACQ_REL || mo == __ATOMIC_SEQ_CST;
}

//Does the memory ordering represent a release operation?
static bool
is_rls(int mo)
{
    return mo == __ATOMIC_RELEASE || mo == __ATOMIC_ACQ_REL || mo == __ATOMIC_SEQ_CST;
}

//Print human readable version of a thread operation
//'mask' is used to truncate (64-bit) addresses to 32 bits to make them more readable
static void
print_result(const struct ver_file_line *fl, uint32_t id, uint32_t step, uintptr_t mask)
{
    printf("Step %2d: thread %u, ", step, id);
    if (fl != NULL)
    {
	assert(fl->file != NULL);
	printf("file %s line %3"PRIuPTR" ", fl->file, fl->line);
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
		else //datasize > 8 (ought to be 16)
		{
		    //Print 128-bit values as two 64-bit hex values separated
		    //by a quote character (')
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
		    //TODO what if result is not a pointer? We shouldn't apply
		    //the 32-bit mask
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
	//NULL return value signifies end of execution
	printf("done\n");
    }
}

//Step a thread (coroutine) through the init/fini function
static bool
exec_coroutine(p64_coroutine_t *cr,
	       intptr_t arg,
	       intptr_t mask)
{
    for (;;)
    {
	intptr_t r = p64_coro_resume(cr, arg);
	if (r == 0)
	{
	    return true;//Thread completed init/fini successfully
	}
	struct ver_file_line *fl = (struct ver_file_line *)r;
	if (VERBOSE || (fl->fmt & V_ABORT) != 0)
	{
	    print_result(fl, 0, arg == RES_INIT ? -1 : NUMSTEPS, mask);
	}
	if ((fl->fmt & V_ABORT) != 0)
	{
	    return false;//Thread aborted execution
	}
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

#define HTAB_SIZE 257
static struct fileline syncs[HTAB_SIZE];
static struct fileline races[HTAB_SIZE];

//Add a (file,line,file,line) tuple to a hash table
static void
fileline_add(struct fileline *fl, const char *file0, uintptr_t line0, const char *file1, uintptr_t line1)
{
    uint32_t h = (line0 + 8192 * line1) % HTAB_SIZE;//Compute a very simple hash value
    uint32_t i = h;
    while ((fl[i].line0 != 0 || fl[i].line1 != 0) &&
	    (fl[i].line0 != line0 || fl[i].line1 != line1))
    {
	i = (i + 1) % HTAB_SIZE;
	if (i == h)
	{
	    fprintf(stderr, "FileLine hash table too small! (HTAB_SIZE=%u)\n", HTAB_SIZE);
	    fflush(stderr);
	    exit(EXIT_FAILURE);
	}
    }
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

//Print all load-acquire/store-release synchronize-with relations
static void
print_syncs(void)
{
    uint32_t cnt = 0;
    for (uint32_t i = 0; i < HTAB_SIZE; i++)
    {
	if (syncs[i].count != 0)
	{
	    printf("load @ %s:%zu synchronizes-with store @ %s:%zu (count %lu)\n",
		    syncs[i].file0, syncs[i].line0,
		    syncs[i].file1, syncs[i].line1,
		    syncs[i].count);
	    cnt++;
	}
    }
    if (cnt == 0)
    {
	printf("No synchronize-with relations detected\n");
    }
}

//Print all detected data races
//A data race is defined as a (regular) read of a memory location that was written
//by a regular write in another thread without any intervening synchronize-with relation
static void
print_races(void)
{
    uint32_t cnt = 0;
    for (uint32_t i = 0; i < HTAB_SIZE; i++)
    {
	if (races[i].count != 0)
	{
	    printf("%s:%zu data-races-with %s:%zu (count %lu)\n",
		    races[i].file0, races[i].line0,
		    races[i].file1, races[i].line1,
		    races[i].count);
	    cnt++;
	}
    }
    if (cnt == 0)
    {
	printf("No data races detected\n");
    }
}

//Check if [addr0:size0] overlaps [addr1:size1]
static bool
check_overlap(uintptr_t addr0, uint32_t size0, uintptr_t addr1, uint32_t size1)
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

//Possible statuses of verification (of some permutation)
enum verify_status { success, interrupted, failed };

//Information about one step of execution
struct step
{
    struct ver_file_line fl;
    uint16_t id;//Thread id that performed this step
    int16_t syncw;//Synchronizes-with this step
};

//Analyze memory accesses in order to detect violations of synchronize-with relations
//E.g. when a regular load matches a regular stor in another thread and there is no
//intervening synchronize-with relation
static bool
analyze_memo(uint32_t id, uint32_t step, struct ver_file_line *fl, struct step trace[])
{
    //Check for read operation, regular or atomic-load-acquire. Ignore atomic-load-relaxed.
    if ((fl->fmt & V_READ) != 0 && fl->memo != __ATOMIC_RELAXED)
    {
	//Try to find matching earlier write
	assert(fl->fmt & V_AD);
	uintptr_t addr = (uintptr_t)fl->addr;
	uint32_t size = fl->fmt & 0xff;
	for (int32_t i = step - 1; i >= 0; i--)
	{
	    uintptr_t addr_i = (uintptr_t)trace[i].fl.addr;
	    uint32_t size_i = trace[i].fl.fmt & 0xff;
	    if ((trace[i].fl.fmt & V_WRITE) && check_overlap(addr, size, addr_i, size_i))
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
		assert(fl->memo == V_REGULAR || trace[i].fl.memo == V_REGULAR);
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

//Verify specific permutation of the specified verification module
static void
verify(const struct ver_funcs *vf, uint64_t permutation, bool analyze, intptr_t mask)
{
    struct step trace[NUMSTEPS + 1] = { 0 };//Set all elements to 0/NULL
    enum verify_status status = interrupted;//Default status
    intptr_t res[NUMCOROS] = { !RET_DONE, !RET_DONE };
    uint64_t p = permutation;//We need a local copy of the permutation, it might be modified
    uint32_t step = 0;
    if (VERBOSE)
    {
	printf("Verifying %s using permutation %#lx\n", vf->name, permutation);
    }
    //Spawn all coroutines
    for (uint32_t id = 0; id < NUMCOROS; id++)
    {
	verify_id = id;
	//Spawn coroutine. It will read its arguments, execute the init function of the
	//verification module and suspend
	intptr_t r = p64_coro_spawn(&CORO[id], coroutine, STACKS[id], sizeof STACKS[id], vf, id);
	if (r != 0)
	{
	    //The verification module performs operations in init phase
	    struct ver_file_line *fl = (struct ver_file_line *)r;
	    if (VERBOSE)
	    {
		print_result(fl, 0, -1, mask);
	    }
	    //Continue execute the init phase
	    if (!exec_coroutine(&CORO[id], RES_INIT, mask))
	    {
		printf("Verification of module %s permutation %#lx failed at init\n",
			vf->name, permutation);
		status = failed;
		goto failure;
	    }
	}
	//Else verification module suspended without doing operations
    }
    //Execute the exec phase according to the permutation
    for (step = 0; step < NUMSTEPS; step++)
    {
	uint32_t id = p & 1;//Compute which coroutine (thread) to execute next
	verify_id = id;//Assign to global variable as well for external use
	//Resume identified coroutine
	intptr_t ret = p64_coro_resume(&CORO[id], RES_EXEC);
	trace[step].id = id;
	trace[step].syncw = -1;//Does not synchronize-with anything (yet)
	//RET_DONE return means thread completed
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
	else//Else thread performed some operation
	{
	    struct ver_file_line *fl = (struct ver_file_line *)ret;
	    res[id] = !RET_DONE;
	    trace[step].fl = *fl;//Save operation in trace
	    if (VERBOSE)
	    {
		print_result(fl, id, step, mask);
	    }
	    //Check for misaligned accesses (e.g. dereferencing marked pointer)
	    if ((fl->fmt & V_AD) != 0 && (fl->fmt & 0xff) != 0)
	    {
		uint32_t datasize = fl->fmt & 0xff;
		if ((uintptr_t)fl->addr % datasize != 0)
		{
		    printf("ERROR: Misaligned address %p for access size %u!\n", fl->addr, datasize);
		    status = failed;
		    step++;//Ensure the last operation is saved in trace
		    goto failure;
		}
	    }
	    if (analyze)
	    {
		//Attempt to detect memory ordering violations (data races)
		if (!analyze_memo(id, step, fl, trace))
		{
		    status = failed;
		    step++;//Ensure the last operation is saved in trace
		    goto failure;
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
		p |= !id;//Set lsb to other thread id
		//Don't remove last used thread id!
	    }
	    //Else check for error reported by verification module or assertion failed,
	    //this aborts current verification
	    else if (fl->fmt & V_ABORT)
	    {
		status = failed;
		step++;//Ensure the error/assert info is saved in trace
		goto failure;
	    }
	    else//One more successful step executed
	    {
		//Remove last used thread id
		p >>= 1;
	    }
	}
    }
    if (status == success)
    {
	//Resume completed coroutine for it to execute the fini function
	if (!exec_coroutine(&CORO[0], RES_FINI, mask))
	{
	    //Some failure or error reported
	    printf("Verification of module %s permutation %#lx failed at fini\n",
		    vf->name, permutation);
	    status = failed;
	    step++;//Ensure the error info is saved in trace
	    goto failure;
	}
	else if (VERBOSE)
	{
	    printf("Verification of module %s permutation %#lx complete after %u steps\n",
		    vf->name, permutation, step);
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
	    printf("Verification of module %s permutation %#lx interrupted after %u steps\n",
		    vf->name, permutation, step);
	    HISTO[INTERRUPTED]++;
	}
	else//status == failed
	{
	    printf("Module %s permutation %#lx step %u: Verification failed\n",
		    vf->name, permutation, step);
	    HISTO[FAILED]++;
	}
	//Print the steps that led to here
	for (uint32_t i = 0; i < step; i++)
	{
	    print_result(&trace[i].fl, trace[i].id, i, mask);
	}
    }
}

//Signal handler for SIGINT (CTRL-C)

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
    //Notify permutation loops that user wants to terminate program
    user_interrupt = true;
}

//xorshift64 is a simple pseudorandom number generator
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
    uint64_t upper = (uint64_t)1 << 32;//Default upper bound of permutations to verify
    uintptr_t mask = ~(uintptr_t)0;//Address mask used when printing
    uint64_t random = 0;//RNG seed and enabler (0: no random sequence)
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
		mask = (uint32_t)mask;//Truncate the address mask to 32 bits
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
			"-u <limit>       Specify sweep upper limit of permutation range\n"
			"-v               Verbose\n"
			"-w               Warnings become failures\n"
		       );
list_vermods:
		fprintf(stderr, "Known verification modules:\n");
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
	fprintf(stderr, "Unknown verification module %s specified\n", argv[optind]);
	goto list_vermods;
    }

    if (!VERBOSE)
    {
	printf("Verifying %s\n", (*vf)->name);
    }
    if (permutation != -1)
    {
	verify(*vf, permutation, analyze, mask);
	if (analyze)
	{
	    print_syncs();
	    print_races();
	}
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
    //Display our statistics
    //Ignore leading and trailing ranges of zero counts
    uint32_t first, last;
    //Find first step with non-zero count
    for (first = 0; first < NUMSTEPS; first++)
    {
	if (HISTO[first] != 0)
	{
	    break;
	}
    }
    //Find last step with non-zero count
    for (last = NUMSTEPS - 1; last > first; last--)
    {
	if (HISTO[last] != 0)
	{
	    break;
	}
    }
    printf("Histogram over number of steps:\n");
    uint64_t succeeded = 0;
    for (uint32_t i = first; i <= last; i++)
    {
	succeeded += HISTO[i];
	printf("%u: %lu\n", i, HISTO[i]);
    }
    printf("succeeded: %lu\n", succeeded);
    printf("interrupted: %lu\n", HISTO[INTERRUPTED]);
    printf("failed: %lu\n", HISTO[FAILED]);
    uint64_t total = succeeded + HISTO[INTERRUPTED] + HISTO[FAILED];
    printf("total: %lu (%#lx)\n", total, total);
    //Display results of memory ordering analysis
    if (analyze)
    {
	print_syncs();
	print_races();
    }

    return EXIT_SUCCESS;
}
