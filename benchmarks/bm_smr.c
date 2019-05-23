//Copyright (c) 2016-2019, ARM Limited. All rights reserved.
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

#include "p64_hazardptr.h"
#include "p64_qsbr.h"
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
    uint32_t idx;
} ALIGNED(CACHE_LINE);


static p64_hpdomain_t *HPDOM = NULL;
static p64_qsbrdomain_t *QSBRDOM = NULL;
static pthread_t tid[MAXTHREADS];
static uint32_t NUMTHREADS = 2;
static int cpus[MAXTHREADS];
static unsigned long CPUFREQ;
static uint64_t AFFINITY = ~0U;
static uint32_t NUMLAPS = 1000000;
static uint32_t NUMOBJS = 100;
static struct object *OBJS;//Array of all objects
static struct object **TABLE;//Pointer to array of object pointers
static uint64_t THREAD_BARRIER ALIGNED(CACHE_LINE);
static bool USEHP = true;
static bool VERBOSE = false;
static sem_t ALL_DONE ALIGNED(CACHE_LINE);
static struct timespec END_TIME;
static uint32_t NUMNULL[MAXTHREADS];
static uint32_t NUMFAIL[MAXTHREADS];

//Wait for my signal to begin
static void
barrier_thr_begin(uint32_t idx)
{
    uint64_t thrmask = 1UL << idx;
    SEVL();
    while (WFE() &&
	   (__atomic_load_n(&THREAD_BARRIER, __ATOMIC_ACQUIRE) & thrmask) == 0)
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
    while (WFE() && __atomic_load_n(&THREAD_BARRIER, __ATOMIC_ACQUIRE) != 0)
    {
	DOZE();
    }
}

static void
delay_loop(uint32_t niter)
{
    for (uint32_t i = 0; i < niter; i++)
    {
	doze();
    }
}

static uint32_t stkptr = 0;
static struct object *stack[1000];

static void
callback(void *_obj)
{
    struct object *obj = _obj;
    //Immediately overwrite the index field with an invalid value
    __atomic_store_n(&obj->idx, ~0U, __ATOMIC_RELAXED);
    if (stkptr == 1000)
    {
	fprintf(stderr, "Object stack full (program limitation)\n");
	exit(EXIT_FAILURE);
    }
    stack[stkptr++] = obj;
}

static void
thr_execute(uint32_t tidx)
{
    uint64_t xor_state[] = { tidx + 1 };//Must be != 0
    if (tidx == 0)
    {
	//Thread 0 is the writer
	//It will remove and insert objects into different slots
	uint32_t numwrites = 0;
	uint32_t numnull = 0;
	while (__atomic_load_n(&THREAD_BARRIER, __ATOMIC_RELAXED) != 1)
	{
	    uint32_t idx = xorshift64star(xor_state) % NUMOBJS;
	    struct object *obj = NULL;
	    if (stkptr != 0)
	    {
		obj = stack[--stkptr];
		obj->idx = idx;
	    }
	    obj = __atomic_exchange_n(&TABLE[idx], obj, __ATOMIC_ACQ_REL);
	    if (obj != NULL)
	    {
		assert(obj->idx == idx);
		if (USEHP)
		{
		    while(!p64_hazptr_retire(obj, callback))
		    {
			(void)p64_hazptr_reclaim();
		    }
		}
		else
		{
		    p64_qsbr_quiescent();
		    while (!p64_qsbr_retire(obj, callback))
		    {
			(void)p64_qsbr_reclaim();
		    }
		}
	    }
	    else
	    {
		numnull++;
	    }
	    numwrites++;
	    //Try to reclaim pending objects
	    if (USEHP)
	    {
		(void)p64_hazptr_reclaim();
	    }
	    else
	    {
		(void)p64_qsbr_reclaim();
	    }
	}
	if (!USEHP)
	{
	    p64_qsbr_quiescent();
	}
	//All reader threads are done so any pending objects should be
	//reclaimable
	for (uint32_t tmo = 0;; tmo++)
	{
	    uint32_t npend;
	    //Clear stack to make room for more objects
	    stkptr = 0;
	    //Try to reclaim pending objects
	    if (USEHP)
		npend = p64_hazptr_reclaim();
	    else
		npend = p64_qsbr_reclaim();
	    //Break if there are no more remaining pending objects
	    if (npend == 0)
		break;
	    if (tmo == 1000000)
	    {
		fprintf(stderr, "%u pending objects never reclaimed\n", npend);
		fflush(stderr);
		abort();
	    }
	}
	NUMNULL[tidx] = numnull;
	NUMFAIL[tidx] = numwrites;
    }
    else
    {
	uint32_t numfail = 0;
	uint32_t numnull = 0;
	for (uint32_t lap = 0; lap < NUMLAPS; lap++)
	{
	    p64_hazardptr_t hp = P64_HAZARDPTR_NULL;
	    uint32_t idx;
	    struct object *obj;
	    if (!USEHP)
		p64_qsbr_acquire();
	    do
	    {
		idx = xorshift64star(xor_state) % NUMOBJS;
		if (USEHP)
		    obj = p64_hazptr_acquire(&TABLE[idx], &hp);
		else
		    obj = __atomic_load_n(&TABLE[idx], __ATOMIC_ACQUIRE);
		if (obj == NULL)
		{
		    numnull++;
		}
	    }
	    while (obj == NULL);
	    delay_loop(10);
	    uint32_t obj_idx = __atomic_load_n(&obj->idx, __ATOMIC_RELAXED);
	    if (obj_idx != idx)
	    {
		numfail++;
	    }
	    if (USEHP)
		p64_hazptr_release(&hp);
	    else
		p64_qsbr_release();
	    if (lap % 10 == 0)
	    {
		//Verify that active/inactive works
		if (USEHP)
		{
		    p64_hazptr_deactivate();
		    delay_loop(1);
		    p64_hazptr_reactivate();
		}
		else
		{
		    p64_qsbr_deactivate();
		    delay_loop(1);
		    p64_qsbr_reactivate();
		}
	    }
	}
	NUMNULL[tidx] = numnull;
	NUMFAIL[tidx] = numfail;
    }
}

static void *
entrypoint(void *arg)
{
    unsigned tidx = (uintptr_t)arg;
    if (USEHP)
	p64_hazptr_register(HPDOM);
    else
	p64_qsbr_register(QSBRDOM);

    //Wait for my signal to start
    barrier_thr_begin(tidx);

    thr_execute(tidx);

    if (USEHP)
	p64_hazptr_unregister();
    else
	p64_qsbr_unregister();

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
benchmark(uint32_t numthreads, uint64_t affinity)
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

    if (affinity != 0 && CPUFREQ == 0)
    {
	unsigned long cpufreq[MAXTHREADS];
	for (uint32_t thr = 0; thr < numthreads; thr++)
	{
	    char s[200];
	    cpufreq[thr] = 0;
	    sprintf(s, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_cur_freq", cpus[thr]);
	    int fd = open(s, O_RDONLY);
	    if (fd != -1)
	    {
		char buf[40];
		int l = read(fd, buf, sizeof buf);
		if (l > 0)
		{
		    cpufreq[thr] = atol(buf);
		}
		close(fd);
	    }
	}
	CPUFREQ = 0;
	for (uint32_t thr = 0; thr < NUMTHREADS; thr++)
	{
	    //printf("Thread %u current CPU frequency %lukHz\n", thr, cpufreq[thr]);
	    CPUFREQ += cpufreq[thr] / NUMTHREADS;
	}
	if (CPUFREQ != 0)
	{
	    printf("Average CPU frequency %lukHz\n", CPUFREQ);
	}
    }

    uint64_t elapsed_ns = ts.tv_sec * 1000000000ULL + ts.tv_nsec - start;
    uint32_t elapsed_s = elapsed_ns / 1000000000ULL;
    printf("%u.%04llu seconds\n",
	    elapsed_s,
	    (elapsed_ns % 1000000000LLU) / 100000LLU);

    printf("Writer  : numnull %u\n", NUMNULL[0]);
    for (uint32_t t = 1; t < NUMTHREADS; t++)
    {
	printf("Reader %u: numnull %u, numfail %u\n",
	       t, NUMNULL[t], NUMFAIL[t]);
    }

    uint32_t numreads = NUMLAPS;
    uint32_t ops_per_sec = 0;
    if (elapsed_ns != 0)
    {
	ops_per_sec = (uint32_t)(1000000000ULL * numreads / elapsed_ns);
	printf("%"PRIu32" reads/second", ops_per_sec);
    }
    else
    {
	printf("INF reads/second");
    }
    if (numreads != 0)//Explicit check against 0 to silence scan-build
    {
	printf(", %"PRIu32" nanoseconds/read\n", (uint32_t)(elapsed_ns / numreads));
    }
    uint32_t numwrites = NUMFAIL[0];
    if (elapsed_ns != 0)
    {
	ops_per_sec = (uint32_t)(1000000000ULL * numwrites / elapsed_ns);
	printf("%"PRIu32" writes/second", ops_per_sec);
    }
    else
    {
	printf("INF writes/second");
    }
    if (numwrites != 0)//Explicit check against 0 to silence scan-build
    {
	printf(", %"PRIu32" nanoseconds/write\n", (uint32_t)(elapsed_ns / numwrites));
    }
}

int
main(int argc, char *argv[])
{
    uint32_t NREFS = 1;
    int c;

    while ((c = getopt(argc, argv, "a:f:l:o:qr:t:v")) != -1)
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
	    case 'f' :
		{
		    CPUFREQ = atol(optarg);
		    break;
		}
	    case 'l' :
		{
		    int numlaps = atoi(optarg);
		    if (numlaps < 1)
		    {
			fprintf(stderr, "Invalid number of laps %d\n", numlaps);
			exit(EXIT_FAILURE);
		    }
		    NUMLAPS = (unsigned)numlaps;
		    break;
		}
	    case 'o' :
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
	    case 'q' :
		USEHP = false;
		break;
	    case 'r' :
		{
		    int numrefs = atoi(optarg);
		    if (numrefs < 0 || numrefs > 32)
		    {
			fprintf(stderr, "Invalid number of references %d\n", numrefs);
			exit(EXIT_FAILURE);
		    }
		    NREFS = (unsigned)numrefs;
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
		fprintf(stderr, "Usage: bm_smr <options>\n"
			"-a <binmask>     CPU affinity mask (default base 2)\n"
			"-f <cpufreq>     CPU frequency in kHz\n"
			"-l <numlaps>     Number of laps\n"
			"-o <numobjs>     Number of objects\n"
			"-q               Use QSBR instead of hazard pointers\n"
			"-r <numrefs>     Number of HP references\n"
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

    printf("%s: %u objects, %u laps, %u thread%s, affinity mask=0x%lx, ",
	    USEHP ? "HP" : "QSBR",
	    NUMOBJS,
	    NUMLAPS,
	    NUMTHREADS,
	    NUMTHREADS != 1 ? "s" : "",
	    AFFINITY);
    fflush(stdout);

    if (USEHP)
    {
	printf("%u HP/thread, ", NREFS);
	HPDOM = p64_hazptr_alloc(5, NREFS);
	if (HPDOM == NULL)
	{
	    fprintf(stderr, "Failed to allocate HP domain\n");
	    exit(EXIT_FAILURE);
	}
    }
    else
    {
	QSBRDOM = p64_qsbr_alloc(5);
	if (QSBRDOM == NULL)
	{
	    fprintf(stderr, "Failed to allocate QSBR domain\n");
	    exit(EXIT_FAILURE);
	}
    }
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
	OBJS[i].idx = i;
	TABLE[i] = &OBJS[i];
    }

    int res = sem_init(&ALL_DONE, 0, 0);
    if (res < 0)
    {
	perror("sem_init"), exit(EXIT_FAILURE);
    }

    if (NUMTHREADS != 0)
    {
	create_threads(NUMTHREADS, AFFINITY);
	benchmark(NUMTHREADS, AFFINITY);
    }

    //Clean up
    for (uint32_t thr = 0; thr < NUMTHREADS; thr++)
    {
	(void)pthread_cancel(tid[thr]);
    }
    (void)sem_destroy(&ALL_DONE);
    free(TABLE);
    free(OBJS);
    if (USEHP)
	p64_hazptr_free(HPDOM);
    else
	p64_qsbr_free(QSBRDOM);
    return 0;
}
