//Copyright (c) 2020, ARM Limited. All rights reserved.
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

#include "p64_mcas.h"
#include "p64_errhnd.h"
#include "p64_hazardptr.h"
#include "p64_qsbr.h"
#include "build_config.h"
#include "common.h"
#include "arch.h"

//Thread priority and scheduling
#define PRIO 1
#define SCHED SCHED_FIFO

//Number of HQ/QSBR reclaim slots
#define NUMRECLAIM 128

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

static p64_hpdomain_t *HPDOM = NULL;
static p64_qsbrdomain_t *QSBRDOM = NULL;
static pthread_t tid[MAXTHREADS];
static uint32_t NUMTHREADS = 2;
static int cpus[MAXTHREADS];
static unsigned long CPUFREQ;
static uint64_t AFFINITY = ~0U;
static uint32_t NUMLAPS = 1000000;
static uint32_t NUMELEMS = 256;
static p64_mcas_ptr_t *TABLE;
static uint64_t THREAD_BARRIER ALIGNED(CACHE_LINE);
static bool QSBR = false;
static bool HELP = false;
static bool VERBOSE = false;
static sem_t ALL_DONE ALIGNED(CACHE_LINE);
static struct timespec END_TIME;
static uint32_t NUMCAS[MAXTHREADS];
static uint32_t NUMFAIL[MAXTHREADS];

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

static int
error_handler(const char *module, const char *cur_err, uintptr_t val)
{
    if (strcmp(module, "mcas") == 0 &&
	strcmp(cur_err, "QSBR reclamation stalled") == 0)
    {
	return P64_ERRHND_RETURN;
    }
    fprintf(stderr, "Error in %s: %s (%p/%lu)\n",
	    module, cur_err, (void *)val, val);
    if (strcmp(module, "hazardptr") == 0)
    {
	fprintf(stderr, "List of active hazard pointers:\n");
	p64_hazptr_dump(stdout);
    }
    return P64_ERRHND_EXIT;
}

static void
thr_execute(uint32_t tidx)
{
    uint64_t xor_state[] = { tidx + 1 };//Must be != 0
    uint32_t numcas = 0, numfail = 0;
    p64_hazardptr_t hp0 = P64_HAZARDPTR_NULL;
    p64_hazardptr_t hp1 = P64_HAZARDPTR_NULL;
    p64_hazardptr_t *hpp0 = QSBR ? NULL : &hp0;
    p64_hazardptr_t *hpp1 = QSBR ? NULL : &hp1;
    for (uint32_t lap = 0; lap < NUMLAPS; lap++)
    {
	uint32_t i0, i1;
	i0 = xorshift64star(xor_state) % NUMELEMS;
	do
	{
	    i1 = xorshift64star(xor_state) % NUMELEMS;
	}
	while (i1 == i0);
	if (QSBR)
	{
	    p64_qsbr_acquire();
	}
	p64_mcas_ptr_t *loc[2], exp[2], new[2];
	loc[0] = &TABLE[i0];
	loc[1] = &TABLE[i1];
	PREFETCH_FOR_WRITE(loc[0]);
	PREFETCH_FOR_WRITE(loc[1]);
	do
	{
	    exp[0] = p64_mcas_read(loc[0], hpp0, HELP);
	    exp[1] = p64_mcas_read(loc[1], hpp1, HELP);
	    //Swap places
	    new[0] = exp[1];
	    new[1] = exp[0];
	    numfail++;
	}
	while (!p64_mcas_casn(2, loc, exp, new, !QSBR));
	numfail--;
	if (QSBR)
	{
	    p64_qsbr_release();
	}
	numcas++;
    }
    if (!QSBR)
    {
	p64_hazptr_release(hpp0);
	p64_hazptr_release(hpp1);
    }
    NUMCAS[tidx] = numcas;
    NUMFAIL[tidx] = numfail;
}

static void *
entrypoint(void *arg)
{
    unsigned tidx = (uintptr_t)arg;
    if (!QSBR)
	p64_hazptr_register(HPDOM);
    else
	p64_qsbr_register(QSBRDOM);

    p64_mcas_init(2 * NUMRECLAIM, 2);

    //Wait for my signal to start
    barrier_thr_begin(tidx);

    thr_execute(tidx);

    if (!QSBR)
    {
	while (p64_hazptr_reclaim() != 0)
	{
	    doze();
	}
	p64_hazptr_unregister();
    }
    else
    {
	while (p64_qsbr_reclaim() != 0)
	{
	    doze();
	}
	p64_qsbr_unregister();
    }
    p64_mcas_fini();

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

static int
compare(const void *va, const void *vb)
{
    const uintptr_t *a = va;
    const uintptr_t *b = vb;
    return *a < *b ? -1 : *a > *b ? 1 : 0;
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

    qsort(TABLE, NUMELEMS, sizeof TABLE[0], compare);
    uint32_t expected = 0;
    for (uint32_t i = 0; i < NUMELEMS; i++)
    {
	if ((uintptr_t)TABLE[i] != expected)
	{
	    printf("Error: TABLE[%u]=%zu, expected %u\n",
		   i, (uintptr_t)TABLE[i], expected);
	    expected = (uintptr_t)TABLE[i];
	    break;
	}
	expected += 4;
    }

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

    for (uint32_t t = 0; t < NUMTHREADS; t++)
    {
	printf("%u: numcas %u, numfail %u\n", t, NUMCAS[t], NUMFAIL[t]);
    }

    uint32_t ops_per_sec = 0;
    if (elapsed_ns != 0)
    {
	ops_per_sec = (uint32_t)(1000000000ULL * NUMLAPS / elapsed_ns);
	printf("%"PRIu32" CASN/second", ops_per_sec);
    }
    else
    {
	printf("INF CASN/second");
    }
    if (NUMLAPS != 0)//Explicit check against 0 to silence scan-build
    {
	printf(", %"PRIu32" nanoseconds/CASN\n", (uint32_t)(elapsed_ns / NUMLAPS));
    }
}

int
main(int argc, char *argv[])
{
    uint32_t NREFS = 10;
    int c;

    while ((c = getopt(argc, argv, "a:e:f:hl:qr:t:v")) != -1)
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
	    case 'e' :
		{
		    int numelems = atoi(optarg);
		    if (numelems < 1)
		    {
			fprintf(stderr, "Invalid number of elements %d\n", numelems);
			exit(EXIT_FAILURE);
		    }
		    NUMELEMS = (unsigned)numelems;
		    break;
		}
	    case 'h' :
		HELP = true;
		break;
	    case 'q' :
		QSBR = true;
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
		fprintf(stderr, "Usage: bm_mcas <options>\n"
			"-a <binmask>     CPU affinity mask (default base 2)\n"
			"-e <numelems>    Number of elements\n"
			"-f <cpufreq>     CPU frequency in kHz\n"
			"-h               Read will help\n"
			"-l <numlaps>     Number of laps\n"
			"-q               Use QSBR (default hazard pointers)\n"
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

    printf("%s: %u elements, %u laps, %u thread%s, help %s, affinity mask=0x%"PRIx64,
	    QSBR ? "QSBR" : "HP",
	    NUMELEMS,
	    NUMLAPS,
	    NUMTHREADS,
	    NUMTHREADS != 1 ? "s" : "",
	    HELP ? "yes" : "no",
	    AFFINITY);
    fflush(stdout);

    p64_errhnd_install(error_handler);
    if (!QSBR)
    {
	printf(", %u HP/thread", NREFS);
	HPDOM = p64_hazptr_alloc(NUMRECLAIM, NREFS);
	if (HPDOM == NULL)
	{
	    fprintf(stderr, "Failed to allocate HP domain\n");
	    exit(EXIT_FAILURE);
	}
    }
    else
    {
	QSBRDOM = p64_qsbr_alloc(NUMRECLAIM);
	if (QSBRDOM == NULL)
	{
	    fprintf(stderr, "Failed to allocate QSBR domain\n");
	    exit(EXIT_FAILURE);
	}
    }
    printf("\n");

    TABLE = aligned_alloc(CACHE_LINE, NUMELEMS * sizeof(p64_mcas_ptr_t));
    if (TABLE == NULL)
    {
	perror("malloc"), exit(EXIT_FAILURE);
    }
    for (uint32_t i = 0; i < NUMELEMS; i++)
    {
	TABLE[i] = (void *)(uintptr_t)(4 * i);
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
    if (QSBR)
    {
	p64_qsbr_free(QSBRDOM);
    }
    else
    {
	p64_hazptr_free(HPDOM);
    }

    return 0;
}
