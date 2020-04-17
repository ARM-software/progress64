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
#include <math.h>
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

#include "p64_skiplock.h"
#include "p64_tktlock.h"
#include "p64_semaphore.h"
#include "build_config.h"
#include "common.h"
#include "arch.h"

//Thread priority and scheduling
#define PRIO 1
#define SCHED SCHED_FIFO

__thread int tidx;

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

union x_b
{
    uint64_t x[8];
    uint8_t b[64];
};

static inline uint64_t
sum_x(volatile union x_b *p)
{
    uint64_t sum = p->x[0] + p->x[1] + p->x[2] + p->x[3] +
		   p->x[4] + p->x[5] + p->x[6] + p->x[7];
    sum = (sum >> 32) + (uint32_t)sum;
    sum = (sum >> 32) + (uint32_t)sum;
    sum = (sum >> 16) + (uint16_t)sum;
    sum = (sum >> 16) + (uint16_t)sum;
    sum = (sum >>  8) + (uint8_t)sum;
    sum = (sum >>  8) + (uint8_t)sum;
    return sum;
}

struct object
{
    p64_skiplock_t sl;
    p64_tktlock_t tktl;
    p64_semaphore_t sem;
    uint32_t otkt ALIGNED(CACHE_LINE);
    uint32_t ocnt ALIGNED(CACHE_LINE);
    volatile union x_b count_rd ALIGNED(CACHE_LINE);
    volatile union x_b count_wr ALIGNED(CACHE_LINE);
} ALIGNED(CACHE_LINE);


static pthread_t tid[MAXTHREADS];
static uint32_t NUMTHREADS = 2;
static int cpus[MAXTHREADS];
static unsigned long CPUFREQ;
static uint64_t AFFINITY = ~0U;
static uint32_t NUMLAPS = 1000000;
static uint32_t NUMOBJS = 1;
static struct object *OBJS;//Pointer to array of aligned locks
static bool VERBOSE = false;
static bool DOCHECKS = false;
static enum { SKIPLOCK, TKT, SEM } LOCKTYPE = -1;
static const char *const type_name[] =
{
    "skiplock", "ticket lock", "semaphore"
};
static const char *const abbr_name[] =
{
    "skiplock", "tkt", "sem"
};
static volatile bool QUIT = false;
static uint64_t THREAD_BARRIER ALIGNED(CACHE_LINE);
static sem_t ALL_DONE ALIGNED(CACHE_LINE);
static struct timespec END_TIME;
static uint32_t NUMFAILWR_WR[MAXTHREADS];
static uint32_t NUMOPSDONE[MAXTHREADS];

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

static void
thr_execute(uint32_t tidx)
{
    uint32_t otkt = 0;
    uint16_t tkt;
    uint32_t numfailwr_wr = 0;
    uint32_t lap;
    uint64_t xor_state[] = { tidx + 1 };//Must be != 0
    for (lap = 0; lap < NUMLAPS && !QUIT; lap++)
    {
	uint32_t idx = xorshift64star(xor_state) % NUMOBJS;
	struct object *obj = &OBJS[idx];
	if (lap % 5 == 0)
	{
	    //Once in a while we realise we don't actually need the mutex
	    switch (LOCKTYPE)
	    {
		case SKIPLOCK :
		    //With skiplock, we can 'skip' our ticket
		    otkt = __atomic_fetch_add(&obj->otkt, 1, __ATOMIC_RELAXED);
		    p64_skiplock_skip(&obj->sl, otkt);
		    break;
		case TKT :
		    //With ticket lock, we need to acquire & release the lock
		    p64_tktlock_acquire(&obj->tktl, &tkt);
		    p64_tktlock_release(&obj->tktl, tkt);
		    break;
		case SEM :
		    //With semaphore, we need to acquire & release the semaphore
		    p64_sem_acquire_n(&obj->sem, NUMTHREADS);
		    p64_sem_release_n(&obj->sem, NUMTHREADS);
		    break;
	    }
	}
	else
	{
	    //Real critical section, need mutual exclusion
	    switch (LOCKTYPE)
	    {
		case SKIPLOCK :
		    otkt = __atomic_fetch_add(&obj->otkt, 1, __ATOMIC_RELAXED);
		    p64_skiplock_acquire(&obj->sl, otkt);
		    break;
		case TKT :
		    p64_tktlock_acquire(&obj->tktl, &tkt);
		    break;
		case SEM :
		    p64_sem_acquire_n(&obj->sem, NUMTHREADS);
		    break;
	    }
	    if (DOCHECKS)
	    {
		obj->count_wr.b[tidx % 64]++;
		if (sum_x(&obj->count_wr) != 1)
		{
		    numfailwr_wr++;//Writer present while writing
		}
	    }
	    delay_loop(50);
	    obj->ocnt++;
	    if (DOCHECKS)
	    {
		if (sum_x(&obj->count_wr) != 1)
		{
		    numfailwr_wr++;//Writer present while writing
		}
		obj->count_wr.b[tidx % 64]--;
	    }
	    switch (LOCKTYPE)
	    {
		case SKIPLOCK :
		    p64_skiplock_release(&obj->sl, otkt);
		    break;
		case TKT :
		    p64_tktlock_release(&obj->tktl, tkt);
		    break;
		case SEM :
		    p64_sem_release_n(&obj->sem, NUMTHREADS);
		    break;
	    }
	}
	delay_loop(10);
    }
    QUIT = true;
    NUMFAILWR_WR[tidx] = numfailwr_wr;
    NUMOPSDONE[tidx] = lap;
}

static void *
entrypoint(void *arg)
{
    //unsigned tidx = (uintptr_t)arg;
    tidx = (uintptr_t)arg;

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
		    cpufreq[thr] = atol(buf) / 1000;//Convert to MHz
		}
		close(fd);
	    }
	}
	CPUFREQ = 0;
	for (uint32_t thr = 0; thr < NUMTHREADS; thr++)
	{
	    //printf("Thread %u current CPU frequency %luMHz\n", thr, cpufreq[thr]);
	    CPUFREQ += cpufreq[thr] / NUMTHREADS;
	}
	if (CPUFREQ != 0)
	{
	    printf("Average CPU frequency %luMHz\n", CPUFREQ);
	}
    }

    uint64_t totalops = 0;
    for (uint32_t t = 0; t < NUMTHREADS; t++)
    {
	printf("%u: ", t);
	if (DOCHECKS)
	{
	    printf("failwr_wr %u, ", NUMFAILWR_WR[t]);
	}
	printf("numops %u\n", NUMOPSDONE[t]);
	totalops += NUMOPSDONE[t];
    }

    uint64_t elapsed_ns = ts.tv_sec * 1000000000ULL + ts.tv_nsec - start;
    uint32_t elapsed_s = elapsed_ns / 1000000000ULL;
    printf("Duration: %u.%04llu seconds\n",
	    elapsed_s,
	    (elapsed_ns % 1000000000LLU) / 100000LLU);

    float fairness = 1.0;
    for (uint32_t t = 0; t < NUMTHREADS; t++)
    {
	if (NUMOPSDONE[t] < NUMLAPS)
	{
	    fairness *= (float)NUMOPSDONE[t] / (float)NUMLAPS;
	}
	else if (NUMOPSDONE[t] > NUMLAPS)
	{
	    fairness *= (float)NUMLAPS / (float)NUMOPSDONE[t];
	}
    }
    fairness = powf(fairness, 1.0 / NUMTHREADS);
    printf("Fairness: %f\n", fairness);

    if (elapsed_ns != 0)
    {
	uint32_t ops_per_sec = (uint32_t)(1000000000ULL * totalops / elapsed_ns);
	printf("%"PRIu32" lock ops/second", ops_per_sec);
    }
    if (totalops != 0)//Explicit check against 0 to silence scan-build
    {
	uint32_t ns_per_op = elapsed_ns / totalops;
	printf(", %"PRIu32" nanoseconds/lock op", ns_per_op);
	if (CPUFREQ != 0)
	{
	    printf(", %"PRIu32" cycles/lock op", (uint32_t)(ns_per_op * CPUFREQ / 1000));
	}
	printf("\n");
    }
}

int
main(int argc, char *argv[])
{
    int c;

    while ((c = getopt(argc, argv, "a:cf:l:o:r:t:v")) != -1)
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
	    case 'c' :
		DOCHECKS = true;
		break;
	    case 'f' :
		CPUFREQ = atoi(optarg);
		break;
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
		fprintf(stderr, "Usage: bm_lock [<options>] <locktype>\n"
			"-a <binmask>     CPU affinity mask (default base 2)\n"
			"-f <megahz>      CPU frequency in MHz\n"
			"-c               Perform lock checks\n"
			"-l <numlaps>     Number of laps\n"
			"-o <numobjs>     Number of objects (locks)\n"
			"-t <numthr>      Number of threads\n"
			"-v               Verbose\n"
			"Lock types: "
		       );
		for (int i = SKIPLOCK; i <= SEM; i++)
		{
		    fprintf(stderr, "%s%c", abbr_name[i], i != SEM ? ' ' : '\n');
		}
		exit(EXIT_FAILURE);
	}
    }
    //Need one
    if (optind != argc - 1)
    {
	goto usage;
    }
    for (int i = SKIPLOCK; i <= SEM; i++)
    {
	if (strcmp(abbr_name[i], argv[optind]) == 0)
	{
	    LOCKTYPE = i;
	    break;
	}
    }
    if ((int)LOCKTYPE == -1)
    {
	goto usage;
    }

    if (NUMOBJS == 0)
    {
	NUMOBJS = NUMTHREADS >= 2 ? NUMTHREADS / 2 : 1;
    }
    printf("%u %s lock%s, %u laps, %u thread%s, affinity mask=0x%"PRIx64"\n",
	    NUMOBJS,
	    type_name[LOCKTYPE],
	    NUMOBJS != 1 ? "s" : "",
	    NUMLAPS,
	    NUMTHREADS,
	    NUMTHREADS != 1 ? "s" : "",
	    AFFINITY);
    fflush(stdout);

    OBJS = aligned_alloc(CACHE_LINE, NUMOBJS * sizeof(struct object));
    if (OBJS == NULL)
    {
	perror("malloc"), exit(EXIT_FAILURE);
    }
    for (uint32_t i = 0; i < NUMOBJS; i++)
    {
	OBJS[i].otkt = 0;
	p64_skiplock_init(&OBJS[i].sl);
	p64_tktlock_init(&OBJS[i].tktl);
	p64_sem_init(&OBJS[i].sem, NUMTHREADS);
	memset((void *)&OBJS[i].count_rd, 0, sizeof(OBJS[i].count_rd));
	memset((void *)&OBJS[i].count_wr, 0, sizeof(OBJS[i].count_wr));
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
    free(OBJS);
    return 0;
}
