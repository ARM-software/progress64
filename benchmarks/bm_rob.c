//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "p64_reorder.h"
#include "p64_buckrob.h"
#include "build_config.h"
#include "common.h"
#include "arch.h"

//Thread priority and scheduling
#define PRIO 1
#define SCHED SCHED_FIFO

static inline uint64_t
xorshift64star(uint64_t xor_state[1])
{
    uint64_t x = xor_state[0];// The state must be seeded with a nonzero value
    x ^= x >> 12; // a
    x ^= x << 25; // b
    x ^= x >> 27; // c
    xor_state[0] = x;
    return x * 0x2545F4914F6CDD1D;
}

struct object
{
    uint32_t sn;
} ALIGNED(CACHE_LINE);


static void *ROB = NULL;
static pthread_t tid[MAXTHREADS];
static uint32_t NUMTHREADS = 2;
static int cpus[MAXTHREADS];
static unsigned long CPUFREQ;
static unsigned long AFFINITY = ~0UL;
static uint32_t NUMOBJS = 10000000;
static uint32_t ROBSIZE = 256;
static uint32_t OOO = 13;
static bool BUCKROB = false;
static struct object *OBJS;//Array of all objects
static struct object **TABLE;//Pointer to array of object pointers
static uint64_t THREAD_BARRIER ALIGNED(CACHE_LINE);
static bool VERBOSE = false;
static sem_t ALL_DONE ALIGNED(CACHE_LINE);
static struct timespec END_TIME;

//Wait for my signal to begin
static void
barrier_thr_begin(uint32_t idx)
{
    uint64_t thrmask = 1UL << idx;
    SEVL();
    while (WFE() && (LDX(&THREAD_BARRIER, __ATOMIC_ACQUIRE) & thrmask) == 0)
    {
	DOZE();
    }
}

//Signal I am done
static void
barrier_thr_done(uint32_t idx)
{
    uint64_t thrmask = 1UL << idx;
    uint64_t left = __atomic_and_fetch(&THREAD_BARRIER,
				       ~thrmask,
				       __ATOMIC_RELEASE);
    if (left == 0)
    {
	//No threads left, we are the last thread to complete
	clock_gettime(CLOCK_MONOTONIC, &END_TIME);
	sem_post(&ALL_DONE);
    }
}

//Signal all threads to begin
static void
barrier_all_begin(uint32_t numthreads)
{
    uint64_t thrmask = (1UL << numthreads) - 1;
    __atomic_store_n(&THREAD_BARRIER, thrmask, __ATOMIC_RELEASE);
    sem_wait(&ALL_DONE);
}

//Wait until all threads are done
static void
barrier_all_wait(uint32_t numthreads)
{
    (void)numthreads;
    SEVL();
    while (WFE() && LDX(&THREAD_BARRIER, __ATOMIC_ACQUIRE) != 0)
    {
	DOZE();
    }
}

#if 0
static void
delay_loop(uint32_t niter)
{
    for (uint32_t i = 0; i < niter; i++)
    {
	doze();
    }
}
#endif

static void
thr_execute(uint32_t tidx)
{
    uint32_t idx = tidx;
    uint32_t inc = NUMTHREADS;
    while (idx < NUMOBJS)
    {
	struct object *obj = TABLE[idx];
	if (BUCKROB)
	    p64_buckrob_release(ROB, obj->sn, (void **)&obj, 1);
	else
	    p64_reorder_release(ROB, obj->sn, (void **)&obj, 1);
	idx += inc;
    }
}

static void *
entrypoint(void *arg)
{
    unsigned tidx = (uintptr_t)arg;

    //Wait for my signal to start
    barrier_thr_begin(tidx);

    thr_execute(tidx);

    //Signal I am done
    barrier_thr_done(tidx);

    return NULL;
}

static void
initialize_attr(pthread_attr_t *attr, int sched, int prio, int cpu)
{
    if (pthread_attr_init(attr) != 0)
    {
	perror("pthread_attr_init"), abort();
    }
    if (cpu != -1)
    {
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(cpu, &cpuset);
	if (pthread_attr_setaffinity_np(attr, sizeof cpuset, &cpuset))
	{
	    perror("pthread_attr_setaffinity_np"), abort();
	}
    }
    if (pthread_attr_setschedpolicy(attr, sched))
    {
	perror("pthread_attr_setschedpolicy"), abort();
    }
    //Get scheduling policy from attr
    if (pthread_attr_setinheritsched(attr, PTHREAD_EXPLICIT_SCHED))
    {
	perror("pthread_attr_setinheritsched"), abort();
    }
    struct sched_param schedp;
    if (sched == SCHED_FIFO || sched == SCHED_RR)
    {
	int err;
	memset(&schedp, 0, sizeof schedp);
	schedp.sched_priority = prio;
	if ((err = pthread_attr_setschedparam(attr, &schedp)) != 0)
	{
	    errno = err;
	    perror("pthread_attr_setschedparam"), abort();
	}
    }
}

static void
create_threads(uint32_t numthr, uint64_t affinity)
{
    void *(*ep)(void *) = entrypoint;
    for (uint32_t thr = 0; thr < numthr; thr++)
    {
	pthread_attr_t pt_attr;
	int cpu = -1;
	if (affinity != 0)
	{
	    cpu = __builtin_ffsl(affinity) - 1;
	    affinity &= ~(1UL << cpu);
	    if (VERBOSE)
		printf("Thread %u on CPU %u\n", thr, cpu);
	}
	cpus[thr] = cpu;
	initialize_attr(&pt_attr, SCHED, PRIO, cpu);
	errno = pthread_create(&tid[thr], &pt_attr, ep, /*arg=*/(void*)(long)thr);
	if (errno != 0)
	{
	    if (errno == EPERM)
	    {
		//Work-around for some platforms that do not support/allow
		//SCHED_FIFO/SCHED_RR
		errno = pthread_attr_destroy(&pt_attr);
		if (errno != 0)
		{
		    perror("pthread_attr_destroy"), exit(EXIT_FAILURE);
		}
		initialize_attr(&pt_attr, SCHED_OTHER, PRIO, cpu);
		errno = pthread_create(&tid[thr], &pt_attr, ep, /*arg=*/(void*)(long)thr);
	    }
	    if (errno != 0)
	    {
		perror("pthread_create"), exit(EXIT_FAILURE);
	    }
	}
	errno = pthread_attr_destroy(&pt_attr);
	if (errno != 0)
	{
	    perror("pthread_attr_destroy"), exit(EXIT_FAILURE);
	}
    }
}

static void
benchmark(uint32_t numthreads)
{
    struct timespec ts;

    //Read starting time
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t start = ts.tv_sec * 1000000000ULL + ts.tv_nsec;
    //Start worker threads
    barrier_all_begin(numthreads);
    //Wait for worker threads to complete
    barrier_all_wait(numthreads);
    //Read end time
    ts = END_TIME;

    uint32_t numops = NUMOBJS;
    uint64_t elapsed_ns = ts.tv_sec * 1000000000ULL + ts.tv_nsec - start;
    uint32_t elapsed_s = elapsed_ns / 1000000000ULL;
    printf("%u operations, %u.%04llu seconds\n",
	    numops,
	    elapsed_s,
	    (elapsed_ns % 1000000000LLU) / 100000LLU);

    uint32_t ops_per_sec = 0;
    if (elapsed_ns != 0)
    {
	ops_per_sec = (uint32_t)(1000000000ULL * numops / elapsed_ns);
	printf("%"PRIu32" ops/second", ops_per_sec);
    }
    else
    {
	printf("INF ops/second");
    }
    if (numops != 0)//Explicit check against 0 to silence scan-build
    {
	printf(", %"PRIu32" nanoseconds/op\n", (uint32_t)(elapsed_ns / numops));
    }
}

static void
rob_callback(void *arg, void *elem, uint32_t sn)
{
    (void)arg;
    (void)elem;
    (void)sn;
    if (elem != NULL)
    {
	static uint32_t nextsn = 0;
	struct object *obj = elem;
	if (obj->sn != sn || nextsn != sn)
	{
	    fprintf(stderr, "error: nextsn %u, sn %u, obj->sn %u\n",
		    nextsn, sn, obj->sn);
	    fflush(stderr);
	    abort();
	}
	nextsn++;
    }
    //Else NULL pointer signifies end of released sequence
}

int
main(int argc, char *argv[])
{
    int c;

    while ((c = getopt(argc, argv, "a:bf:n:o:r:t:v")) != -1)
    {
	switch (c)
	{
	    case 'a' :
		if (optarg[0] == '0' && optarg[1] == 'x')
		{
		    AFFINITY = strtoul(optarg + 2, NULL, 16);
		}
		else
		{
		    AFFINITY = strtoul(optarg, NULL, 2);
		}
		break;
	    case 'b' :
		BUCKROB = true;
		break;
	    case 'f' :
		{
		    CPUFREQ = atol(optarg);
		    break;
		}
	    case 'n' :
		{
		    int numobjs = atoi(optarg);
		    if (numobjs < 1)
		    {
			fprintf(stderr, "Invalid number of objects %d\n", numobjs);
			exit(EXIT_FAILURE);
		    }
		    NUMOBJS = (unsigned)numobjs;
		    break;
		}
	    case 'o' :
		{
		    int ooo = atoi(optarg);
		    if (ooo < 0)
		    {
			fprintf(stderr, "Invalid out-of-orderness %d\n", ooo);
			exit(EXIT_FAILURE);
		    }
		    OOO = (unsigned)ooo;
		    break;
		}
	    case 'r' :
		{
		    int robsize = atoi(optarg);
		    if (robsize < 1)
		    {
			fprintf(stderr, "Invalid ROB size %d\n", robsize);
			exit(EXIT_FAILURE);
		    }
		    ROBSIZE = (unsigned)robsize;
		    break;
		}
	    case 't' :
		{
		    int numthreads = atoi(optarg);
		    if (numthreads < 1 || numthreads > MAXTHREADS)
		    {
			fprintf(stderr, "Invalid number of threads %d\n", numthreads);
			exit(EXIT_FAILURE);
		    }
		    NUMTHREADS = (unsigned)numthreads;
		    break;
		}
	    case 'v' :
		VERBOSE = true;
		break;
	    default :
usage :
		fprintf(stderr, "Usage: bm_reorder <options>\n"
			"-a <binmask>     CPU affinity mask (default base 2)\n"
			"-b               Use buckrob\n"
			"-f <cpufreq>     CPU frequency in kHz\n"
			"-n <numobjs>     Number of objects\n"
			"-o <oooness>     Out-of-orderness\n"
			"-r <robsize>     Size of reorder buffer\n"
			"-t <numthr>      Number of threads\n"
			"-v               Verbose\n"
		       );
		exit(EXIT_FAILURE);
	}
    }
    if (optind != argc)
    {
	goto usage;
    }

    printf("%s: robsize %u, %u objects, out-of-orderness %u, "
	   "%u thread%s, affinity mask=0x%lx\n",
	    "reorder",
	    ROBSIZE,
	    NUMOBJS,
	    OOO,
	    NUMTHREADS,
	    NUMTHREADS != 1 ? "s" : "",
	    AFFINITY);

    OBJS = aligned_alloc(CACHE_LINE, NUMOBJS * sizeof(struct object));
    if (OBJS == NULL)
    {
	perror("malloc"), exit(EXIT_FAILURE);
    }
    TABLE = aligned_alloc(CACHE_LINE, NUMOBJS * sizeof(struct object *));
    if (TABLE == NULL)
    {
	perror("malloc"), exit(EXIT_FAILURE);
    }
    for (uint32_t i = 0; i < NUMOBJS; i++)
    {
	OBJS[i].sn = i;
	TABLE[i] = &OBJS[i];
    }
    uint64_t xor_state[] = { 1 };//Must be != 0
    for (uint32_t i = 0; i < NUMOBJS - OOO; i++)
    {
	uint64_t rand = xorshift64star(xor_state);
	uint32_t move = rand % (OOO + 1);
	assert(i + move < NUMOBJS);
	SWAP(TABLE[i], TABLE[i + move]);
    }
    uint64_t sumooo = 0;
    uint32_t maxooo = 0;
    for (uint32_t i = 0; i < NUMOBJS; i++)
    {
	int32_t ooo = TABLE[i]->sn - i;
	uint32_t absooo = ooo < 0 ? -ooo : ooo;
	if (absooo > maxooo) maxooo = absooo;
	sumooo += absooo;
    }
    printf("Average ooo %.1f, maxooo %u\n", (float)sumooo / NUMOBJS, maxooo);
    if (maxooo > ROBSIZE)
    {
	printf("Warning: maxooo (%u) >= robsize (%u)\n", maxooo, ROBSIZE);
    }

    if (BUCKROB)
    {
	ROB = p64_buckrob_alloc(ROBSIZE, true, rob_callback, NULL);
	if (ROB == NULL)
	    perror("p64_buckrob_alloc"), abort();
    }
    else
    {
	ROB = p64_reorder_alloc(ROBSIZE, true, rob_callback, NULL);
	if (ROB == NULL)
	    perror("p64_reorderuckrob_alloc"), abort();
    }
    int res = sem_init(&ALL_DONE, 0, 0);
    if (res < 0)
    {
	perror("sem_init"), exit(EXIT_FAILURE);
    }

    if (NUMTHREADS != 0)
    {
	create_threads(NUMTHREADS, AFFINITY);
	benchmark(NUMTHREADS);
    }

    //Clean up
    for (uint32_t thr = 0; thr < NUMTHREADS; thr++)
    {
	(void)pthread_cancel(tid[thr]);
    }
    (void)sem_destroy(&ALL_DONE);
    if (BUCKROB)
	p64_buckrob_free(ROB);
    else
	p64_reorder_free(ROB);
    free(TABLE);
    free(OBJS);
    return 0;
}
