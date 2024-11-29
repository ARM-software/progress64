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

#include "p64_ringbuf.h"
#include "p64_lfring.h"
#include "p64_buckring.h"
#include "p64_stack.h"
#include "p64_lfstack.h"
#include "p64_hazardptr.h"
#include "p64_msqueue.h"
#include "p64_blkring.h"
#include "build_config.h"
#include "common.h"
#include "arch.h"

//Thread priority and scheduling
#define PRIO 1
#define SCHED SCHED_FIFO

#define RINGSIZE 1024
#define MAXRINGBUFS 100
#define MAXELEMS 100000

/******************************************************************************
 * Ring buffer element
 *****************************************************************************/

struct element_s
{
    p64_stack_elem_t *elem;
    p64_lfstack_elem_t *lfelem;
    uint32_t lap;
    uint32_t number;
};

static inline struct element_s *
elem_alloc(void)
{
    return aligned_alloc(CACHE_LINE, sizeof(struct element_s));
}

#define NUMRAND 65536
static uint16_t randtable[NUMRAND];
static pthread_t tid[MAXTHREADS];
static uint32_t NUMTHREADS = 0;
static uint32_t MAXNUMTHREADS = 4;
static uint32_t NUMRINGBUFS = 1;
static p64_hpdomain_t *HPD = NULL;
static int cpus[MAXTHREADS];
static uint32_t FAILENQ[MAXTHREADS];
static uint32_t FAILDEQ[MAXTHREADS];
static unsigned long CPUFREQ;
static uint32_t RESULT_OPS[MAXTHREADS];
static uint32_t RESULT_OVH[MAXTHREADS];
static bool DOUBLESTEP = false;
static uint64_t AFFINITY = ~0U;
static void *RINGBUFS[MAXRINGBUFS];
static uint32_t NUMLAPS = 10000;
static uint32_t NUMELEMS = 256;
static struct element_s *ELEMS[MAXELEMS];
static uint32_t NUMCOMPLETED ALIGNED(CACHE_LINE);
static uint64_t THREAD_BARRIER ALIGNED(CACHE_LINE);
static sem_t ALL_DONE;
static struct timespec END_TIME;
static bool VERBOSE = false;
static uint32_t WORKCYCLES = 400;
enum { classic, lfring, buckring, stack, lfstack, blkring, msqueue } RING_IMPL;

static p64_stack_t *
stack_alloc(uint32_t aba)
{
     p64_stack_t *stk = aligned_alloc(CACHE_LINE, sizeof(p64_stack_t));
     if (stk == NULL)
     {
	 perror("malloc"), abort();
     }
     p64_stack_init(stk, aba);
     return stk;
}

static void
stack_free(p64_stack_t *stk)
{
    free(stk);
}

static p64_lfstack_t *
lfstack_alloc(void)
{
     p64_lfstack_t *stk = aligned_alloc(CACHE_LINE, sizeof(p64_lfstack_t));
     if (stk == NULL)
     {
	 perror("malloc"), abort();
     }
     p64_lfstack_init(stk);
     return stk;
}

static void
lfstack_free(p64_lfstack_t *stk)
{
    free(stk);
}

struct msqueue
{
    _Alignas(CACHE_LINE) p64_ptr_tag_t qhead;
    _Alignas(CACHE_LINE) p64_ptr_tag_t qtail;
};

static struct msqueue *
msqueue_alloc(uint32_t aba)
{
     struct msqueue *msq = aligned_alloc(CACHE_LINE, sizeof(struct msqueue));
     if (msq == NULL)
     {
	 perror("malloc"), abort();
     }
     p64_msqueue_elem_t *node = aligned_alloc(CACHE_LINE,
					      sizeof(p64_msqueue_elem_t) +
					      sizeof(void *));
     if (node == NULL)
     {
	 perror("malloc"), abort();
     }
     node->max_size = sizeof(void *);
     p64_msqueue_init(&msq->qhead, &msq->qtail, aba, node);
     return msq;
}

static void
msqueue_free(struct msqueue *msq)
{
    free(p64_msqueue_fini(&msq->qhead, &msq->qtail));
}

//Wait for my signal to begin
static void barrier_thr_begin(uint32_t idx)
{
    uint64_t thrmask = 1UL << idx;
    while ((LDX(&THREAD_BARRIER, __ATOMIC_ACQUIRE) & thrmask) == 0)
    {
	WFE();
    }
}

//Signal I am done
static void barrier_thr_done(uint32_t idx)
{
    uint64_t thrmask = 1UL << idx;
    uint64_t left = __atomic_and_fetch(&THREAD_BARRIER, ~thrmask, __ATOMIC_RELEASE);
    if (left == 0)
    {
	//No threads left, we are the last thread to complete
	clock_gettime(CLOCK_MONOTONIC, &END_TIME);
	sem_post(&ALL_DONE);
    }
}

//Signal all threads to begin
static void barrier_all_begin(uint32_t numthreads)
{
    uint64_t thrmask = numthreads < 64 ? (1UL << numthreads) - 1 : ~0UL;
    __atomic_store_n(&THREAD_BARRIER, thrmask, __ATOMIC_RELEASE);
    sem_wait(&ALL_DONE);
}

//Wait until all threads are done
static void barrier_all_wait(uint32_t numthreads)
{
    (void)numthreads;
    while (LDX(&THREAD_BARRIER, __ATOMIC_ACQUIRE) != 0)
    {
	WFE();
    }
}

static THREAD_LOCAL p64_msqueue_elem_t *msq_freelist = NULL;

static bool
enqueue(void *rb, void *elem)
{
    switch (RING_IMPL)
    {
	case classic :
	    return p64_ringbuf_enqueue(rb, &elem, 1) == 1;
	case lfring :
	    return p64_lfring_enqueue(rb, &elem, 1) == 1;
	case buckring :
	    return p64_buckring_enqueue(rb, &elem, 1) == 1;
	case stack :
	    p64_stack_enqueue((p64_stack_t *)rb, elem);
	    return true;
	case lfstack :
	    p64_lfstack_enqueue((p64_lfstack_t *)rb, elem);
	    return true;
	case blkring :
	    p64_blkring_enqueue(rb, &elem, 1);
	    return true;
	case msqueue :
	{
	    p64_msqueue_elem_t *node = msq_freelist;
	    if (node == NULL)
	    {
		fprintf(stderr, "msq_freelist is empty\n");
		fflush(stderr);
		abort();
	    }
	    assert(node->next.tag == ~0UL);
	    msq_freelist = node->next.ptr;
	    struct msqueue *msq = (struct msqueue *)rb;
	    p64_msqueue_enqueue(&msq->qhead, &msq->qtail, node, &elem, sizeof elem);
	    return true;
	}
    }
    abort();
}

static void
reclaim_node(void *_node)
{
    p64_msqueue_elem_t *node = _node;
    assert(node->next.tag == ~0UL);
    node->next.ptr = msq_freelist;
    msq_freelist = node;
}

static void *
dequeue(void *rb)
{
    void *elem;
    uint32_t idx;
    switch (RING_IMPL)
    {
	case classic :
	    if (p64_ringbuf_dequeue(rb, &elem, 1, &idx) != 0)
	    {
		return elem;
	    }
	    break;
	case lfring :
	    if (p64_lfring_dequeue(rb, &elem, 1, &idx) != 0)
	    {
		return elem;
	    }
	    break;
	case buckring :
	    if (p64_buckring_dequeue(rb, &elem, 1, &idx) != 0)
	    {
		return elem;
	    }
	    break;
	case stack :
	    return p64_stack_dequeue((p64_stack_t *)rb);
	case lfstack :
	    return p64_lfstack_dequeue((p64_lfstack_t *)rb);
	case blkring :
	    p64_blkring_dequeue(rb, &elem, 1, &idx);
	    return elem;
	case msqueue :
	{
	    struct msqueue *msq = (struct msqueue *)rb;
	    uint32_t sizeof_elem = sizeof elem;
	    p64_msqueue_elem_t *node = p64_msqueue_dequeue(&msq->qhead,
							   &msq->qtail,
							   &elem,
							   &sizeof_elem);
	    if (node != NULL)
	    {
		assert(node->next.tag == ~0UL);
		assert(sizeof_elem == sizeof elem);
		if (HPD)
		{
		    while(!p64_hazptr_retire(node, reclaim_node))
		    {
			(void)p64_hazptr_reclaim();
		    }
		}
		else//Reclaim node immediately
		{
		    reclaim_node(node);
		}
		return elem;
	    }
	    break;
	}
    }
    return NULL;
}

static inline void
work(uint32_t iter)
{
#ifdef __aarch64__
    uint32_t one = 1;
    __asm __volatile("   cbz %0,1f;"
		     "   .align 4;"
		     "0: sub %0,%0,%1;"
		     "   cbnz %0,0b;"
		     "1:"
		     : "+r" (iter)
		     : "r" (one)
		     : "cc");
#endif
}

static void thr_execute(uint32_t tidx)
{
    //Enqueue elements from elems array onto ringbuf 0
    for (unsigned i = tidx; i < NUMELEMS; i+= NUMTHREADS)
    {
	struct element_s *elem = ELEMS[i];
	elem->lap = 0;
	elem->number = i;
	if (!enqueue(RINGBUFS[0], ELEMS[i]))
	{
	    fprintf(stderr, "Failed initial enqueue of element %u\n", i);
	    fflush(NULL);
	    abort();
	}
    }

    //Move elements from ringbuf N to ringbuf N+1
    unsigned ptr = (1000 * tidx) % NUMRAND;
    uint32_t failenq = 0, faildeq = 0;
    while (__atomic_load_n(&NUMCOMPLETED, __ATOMIC_RELAXED) != NUMELEMS)
    {
	uint32_t q = randtable[ptr];
	ptr = (ptr + 1) % NUMRAND;
	struct element_s *elem;
	for (;;)
	{
	    elem = dequeue(RINGBUFS[q]);
	    if (elem == (void *)1)
	    {
		if (__atomic_load_n(&NUMCOMPLETED, __ATOMIC_RELAXED) == NUMELEMS)
		    goto done;
		continue;
	    }
	    if (elem != NULL)
		break;
	    if (__atomic_load_n(&NUMCOMPLETED, __ATOMIC_RELAXED) == NUMELEMS)
		goto done;
	    faildeq++;
#if 0
	    if (faildeq > 100000000)
	    {
		printf("Thread %u dequeue failed\n", tidx);
		print_ringbuf(RINGBUFS[q], NUMTHREADS);
		printf("NUMCOMPLETED=%u\n", NUMCOMPLETED);
		abort();
	    }
#endif
	    q = (q + 1) % NUMRINGBUFS;
	}
	assert(elem != NULL);

	work(WORKCYCLES);
	if (++elem->lap != NUMLAPS)
	{
	    q = randtable[ptr];
	    ptr = (ptr + 1) % NUMRAND;
	    for (;;)
	    {
		bool success = enqueue(RINGBUFS[q], elem);
		if (success)
		    break;
		q = (q + 1) % NUMRINGBUFS;
		//printf("%u: enqueue() to ringbuf %u FAILED\n", tidx, q);
		failenq++;
#if 0
		if (failenq > 100000000)
		{
		    printf("Thread %u enqueue failed\n", tidx);
		    print_ringbuf(RINGBUFS[q], NUMTHREADS);
		    printf("NUMCOMPLETED=%u\n", NUMCOMPLETED);
		    abort();
		}
#endif
	    }
	    //printf("%u: enqueue(%u) to ringbuf %u\n", tidx, elem->number, q);
	}
	else//Element has endured all laps
	{
	    __atomic_fetch_add(&NUMCOMPLETED, 1, __ATOMIC_RELAXED);
	}
	work(WORKCYCLES);
    }
done:
    FAILENQ[tidx] = failenq;
    FAILDEQ[tidx] = faildeq;
    if (RING_IMPL == blkring)
    {
	for (uint32_t i = 0; i < NUMRINGBUFS; i++)
	{
	    void *elem = (void *)1;
	    p64_blkring_enqueue(RINGBUFS[i], &elem, 1);
	}
    }
}

static void *entrypoint(void *arg)
{
    unsigned tidx = (uintptr_t)arg;

    if (HPD != NULL)
    {
	p64_hazptr_register(HPD);
    }

    for (;;)
    {
	if (RING_IMPL == msqueue)
	{
	    assert(sizeof(p64_msqueue_elem_t) + sizeof(void *) <= CACHE_LINE);
	    uint32_t nnodes = tidx == 0 ? NUMELEMS + 10 : 10;
	    size_t nodesz = sizeof(p64_msqueue_elem_t) + sizeof(void *);
	    p64_msqueue_elem_t *prev = NULL;
	    for (uint32_t i = 0; i < nnodes; i++)
	    {
		p64_msqueue_elem_t *node = aligned_alloc(CACHE_LINE, nodesz);
		if (node == NULL)
		{
		    perror("malloc"), abort();
		}
		node->next.tag = ~0UL;
		node->next.ptr = prev;
		node->max_size = sizeof(void *);
		prev = node;
	    }
	    msq_freelist = prev;
	}

	//Wait for my signal to start
	barrier_thr_begin(tidx);

	thr_execute(tidx);

	//Signal I am done
	barrier_thr_done(tidx);

	if (RING_IMPL == msqueue)
	{
	    p64_msqueue_elem_t *node = msq_freelist;
	    while (node != NULL)
	    {
		p64_msqueue_elem_t *next = node->next.ptr;
		assert(node->next.tag == ~0UL);
		assert(node->max_size == sizeof(void *));
		free(node);
		node = next;
	    }
	}
    }

    if (HPD != NULL)
    {
	p64_hazptr_unregister();
    }

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

static void create_threads(uint32_t numthr, uint64_t affinity)
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
	    if (DOUBLESTEP)
		cpu *= 2;
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

static char *percent(uint32_t x, uint32_t y)
{
    if (x != 0)
    {
	static char buf[80];
	uint32_t z = 1000 * x / y;
	sprintf(buf, "(%u.%u%%)", z / 10, z % 10);
	return buf;
    }
    else
    {
	return "";
    }
}

static void benchmark(uint32_t numthreads, uint64_t affinity)
{
    struct timespec ts;

    NUMTHREADS = numthreads;
    NUMCOMPLETED = 0;

    //Read starting time
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t start = ts.tv_sec * 1000000000ULL + ts.tv_nsec;
    //Start worker threads
    barrier_all_begin(numthreads);
    //Wait for worker threads to complete
    barrier_all_wait(numthreads);
    //Read end time
    ts = END_TIME;

    if (RING_IMPL == blkring)
    {
	for (unsigned i = 0; i < NUMRINGBUFS; i++)
	{
	    uint32_t index;
	    void *ev[NUMTHREADS];
	    //Free any remaining sentinels
	    p64_blkring_dequeue_nblk(RINGBUFS[i], ev, NUMTHREADS, &index);
	}
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
	    if (VERBOSE)
	    {
		printf("Thread %u current CPU frequency %lukHz\n",
			thr, cpufreq[thr]);
	    }
	    CPUFREQ += cpufreq[thr] / NUMTHREADS;
	}
	if (CPUFREQ != 0)
	{
	    printf("Average CPU frequency %lukHz\n", CPUFREQ);
	}
    }

    uint32_t numops = NUMELEMS * NUMLAPS;

    if (VERBOSE)
    {
	printf("Total %u operations\n", numops);
	uint64_t failenq = 0, faildeq = 0;
	for (uint32_t i = 0; i < NUMTHREADS; i++)
	{
	    failenq += FAILENQ[i];
	    faildeq += FAILDEQ[i];
	}
	printf("fail_count[enq]=%"PRIu64" %s\n", failenq, percent(failenq, numops));
	printf("fail_count[deq]=%"PRIu64" %s\n", faildeq, percent(faildeq, numops));
    }

    uint64_t elapsed_ns = ts.tv_sec * 1000000000ULL + ts.tv_nsec - start;
    uint32_t elapsed_s = elapsed_ns / 1000000000ULL;
    printf("%u threads: %u.%04llu seconds, ",
	    numthreads,
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
	printf(", %"PRIu32" nanoseconds/update", (uint32_t)(elapsed_ns / numops));
	if (CPUFREQ != 0)
	{
	    uint64_t cycles = numthreads * elapsed_ns * CPUFREQ / 1000000ULL;
	    printf(", %"PRIu32" cycles/update", (uint32_t)(cycles / numops));
	    RESULT_OVH[numthreads - 1] =
		(elapsed_ns * CPUFREQ / 1000000ULL) / numops;
	}
	else
	{
	    RESULT_OVH[numthreads - 1] = elapsed_ns / numops;
	}
    }
    printf("\n");
    RESULT_OPS[numthreads - 1] = ops_per_sec;
}

int main(int argc, char *argv[])
{
    int rbmode = 0;
    int c;

    while ((c = getopt(argc, argv, "A:a:e:f:l:m:pr:t:T:vw:")) != -1)
    {
	switch (c)
	{
	    case 'A' :
		DOUBLESTEP = true;
		break;
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
	    case 'e' :
		{
		    int numelems = atoi(optarg);
		    if (numelems < 1 || numelems > MAXELEMS)
		    {
			fprintf(stderr, "Invalid number of elements %d\n", numelems);
			exit(EXIT_FAILURE);
		    }
		    NUMELEMS = (unsigned)numelems;
		    break;
		}
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
	    case 'm' :
		{
		    rbmode = atoi(optarg);
		    if (rbmode < 0 || rbmode > 16)
		    {
			fprintf(stderr, "Invalid ring buffer mode %d\n", rbmode);
			exit(EXIT_FAILURE);
		    }
		    break;
		}
	    case 'r' :
		{
		    int numringbufs = atoi(optarg);
		    if (numringbufs < 1 || numringbufs > MAXRINGBUFS)
		    {
			fprintf(stderr, "Invalid number of ringbufs %d\n", numringbufs);
			exit(EXIT_FAILURE);
		    }
		    NUMRINGBUFS = (unsigned)numringbufs;
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
		    MAXNUMTHREADS = 0;
		    break;
		}
	    case 'T' :
		{
		    int maxnumthreads = atoi(optarg);
		    if (maxnumthreads < 1 || maxnumthreads > MAXTHREADS)
		    {
			fprintf(stderr, "Invalid number of maxnumthreads %d\n", maxnumthreads);
			exit(EXIT_FAILURE);
		    }
		    MAXNUMTHREADS = (unsigned)maxnumthreads;
		    NUMTHREADS = 0;
		    break;
		}
	    case 'v' :
		VERBOSE = true;
		break;
	    case 'w' :
		{
		    int workcycl = atoi(optarg);
		    if (workcycl < 0)
		    {
			fprintf(stderr, "Invalid number of work cycles %d\n", workcycl);
			exit(EXIT_FAILURE);
		    }
		    WORKCYCLES = (unsigned)workcycl;
		    break;
		}
	    default :
usage :
		fprintf(stderr, "Usage: bm_ringbuf <options>\n"
			"-a <binmask>     CPU affinity mask (default base 2)\n"
			"-e <numelems>    Number of elements\n"
			"-f <cpufreq>     CPU frequency in kHz\n"
			"-l <numlaps>     Number of laps\n"
			"-m <mode>        Ring buffer mode\n"
			"-r <numringbufs> Number of ring buffers\n"
			"-t <numthr>      Number of threads\n"
			"-T <numthr>      Iterate over 1..T number of threads\n"
			"-v               Verbose\n"
			"-w <workcycles>  Number of work cycles\n"
		       );
		fprintf(stderr, "mode 0: blocking enqueue/blocking dequeue\n");
		fprintf(stderr, "mode 1: blocking enqueue/non-blocking dequeue\n");
		fprintf(stderr, "mode 2: non-blocking enqueue/blocking dequeue\n");
		fprintf(stderr, "mode 3: non-blocking enqueue/non-blocking dequeue\n");
		fprintf(stderr, "mode 4: blocking enqueue/lock-free dequeue\n");
		fprintf(stderr, "mode 5: non-blocking enqueue/lock-free dequeue\n");
		fprintf(stderr, "mode 6: lfring\n");
		fprintf(stderr, "mode 7: buckring\n");
		fprintf(stderr, "modes 8-11: Treiber stack w. aba workarounds\n");
		fprintf(stderr, "modes 12-14: M&S queue\n");
		fprintf(stderr, "mode 15: Treiber lfstack w. backoff\n");
		fprintf(stderr, "mode 16: blocking ring buffer\n");
		exit(EXIT_FAILURE);
	}
    }
    if (optind != argc)
    {
	goto usage;
    }

    switch (rbmode)
    {
	case 0 :
	case 1 :
	case 2 :
	case 3 :
	case 4 :
	case 5 :
	    RING_IMPL = classic;
	    break;
	case 6 :
	    RING_IMPL = lfring;
	    break;
	case 7 :
	    RING_IMPL = buckring;
	    break;
	case 8 :
	case 9 :
	case 10 :
	case 11:
	    RING_IMPL = stack;
	    break;
	case 12:
	case 13:
	case 14:
	    RING_IMPL = msqueue;
	    break;
	case 15:
	    RING_IMPL = lfstack;
	    break;
	case 16:
	    RING_IMPL = blkring;
	    break;
	default :
	    abort();
    }
    printf("%u elems, %u ringbuf%s, ",
	    NUMELEMS,
	    NUMRINGBUFS,
	    NUMRINGBUFS != 1 ? "s" : "");
    int aba_mode = 0;
    if (RING_IMPL == msqueue)
    {
	const char *const aba[] = { "lock", "tag", "smr" };
	printf("M&S queue (aba %s), ", aba[rbmode - 12]);
	aba_mode = rbmode - 12;
    }
    else if (RING_IMPL == stack)
    {
	const char *const aba[] = { "lock", "tag", "smr", "llsc" };
	printf("Treiber stack (aba %s), ", aba[rbmode - 8]);
	aba_mode = rbmode - 8;
    }
    else if (RING_IMPL == lfstack)
    {
	printf("lfstack+backoff, ");
    }
    else if (RING_IMPL == buckring)
    {
	printf("buckring, ");
    }
    else if (RING_IMPL == lfring)
    {
	printf("lfring, ");
    }
    else if (RING_IMPL == blkring)
    {
	printf("blkring, ");
    }
    else
    {
	printf("mode enq=%c/deq=%c, ",
	       (rbmode & 1) ? 'N' : 'B', //Enqueue
	       (rbmode & 4) ? 'L' : (rbmode & 2) ? 'N' : 'B');//Dequeue
    }
    printf("%u laps, %u work cycles, %u thread%s, affinity mask=0x%"PRIx64"\n",
	    NUMLAPS,
	    WORKCYCLES,
	    NUMTHREADS,
	    NUMTHREADS != 1 ? "s" : "",
	    AFFINITY);

    for (unsigned i = 0; i < NUMRAND; i += 2)
    {
	unsigned r = rand();
	randtable[i    ] = (r & 0xffff) % NUMRINGBUFS;
	randtable[i + 1] = (r >> 16) % NUMRINGBUFS;
    }
    if (aba_mode == P64_ABA_SMR)
    {
        HPD = p64_hazptr_alloc(10, 2);
        p64_hazptr_register(HPD);
    }

    for (unsigned i = 0; i < NUMRINGBUFS; i++)
    {
	uint32_t flags = 0;
	flags |= (rbmode & 1) ? P64_RINGBUF_F_NBENQ : 0;
	flags |= (rbmode & 2) ? P64_RINGBUF_F_NBDEQ : 0;
	flags |= (rbmode & 4) ? P64_RINGBUF_F_LFDEQ : 0;
	void *q = NULL;
	switch (RING_IMPL)
	{
	    case classic :
		q = p64_ringbuf_alloc(RINGSIZE, flags, sizeof(void *));
		break;
	    case lfring :
		q = p64_lfring_alloc(RINGSIZE, 0);
		break;
	    case buckring :
		q = p64_buckring_alloc(RINGSIZE, 0);
		break;
	    case stack :
		q = stack_alloc(aba_mode);
		break;
	    case lfstack :
		q = lfstack_alloc();
		break;
	    case blkring :
		q = p64_blkring_alloc(RINGSIZE);
		break;
	    case msqueue :
		q = msqueue_alloc(aba_mode);
		break;
	    default :
		abort();
	}
	if (q == NULL)
	{
	    fprintf(stderr, "Failed to create ring buffer\n");
	    exit(EXIT_FAILURE);
	}
	RINGBUFS[i] = q;
    }

    for (unsigned i = 0; i < NUMELEMS; i++)
    {
	struct element_s *elem = elem_alloc();
	if (elem == NULL)
	{
	    fprintf(stderr, "Failed to allocate element\n");
	    exit(EXIT_FAILURE);
	}
	ELEMS[i] = elem;
    }
    int res = sem_init(&ALL_DONE, 0, 0);
    if (res < 0)
    {
	perror("sem_init"), exit(EXIT_FAILURE);
    }

    if (MAXNUMTHREADS != 0)
    {
	create_threads(MAXNUMTHREADS, AFFINITY);
	for (uint32_t numthr = 1; numthr <= MAXNUMTHREADS; numthr++)
	{
	    benchmark(numthr, AFFINITY);
	}
	printf("(enq+deq)/s ");
	for (uint32_t numthr = 1; numthr <= MAXNUMTHREADS; numthr++)
	{
	    printf("%u%c",
		   RESULT_OPS[numthr - 1],
		   numthr < MAXNUMTHREADS ? ',' : '\n');
	}
	if (CPUFREQ != 0)
	{
	    printf("ovh/cycles ");
	}
	else
	{
	    printf("ovh/ns ");
	}
	for (uint32_t numthr = 1; numthr <= MAXNUMTHREADS; numthr++)
	{
	    printf("%u%c",
		   RESULT_OVH[numthr - 1],
		   numthr < MAXNUMTHREADS ? ',' : '\n');
	}
    }
    else if (NUMTHREADS != 0)
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
    for (unsigned i = 0; i < NUMELEMS; i++)
    {
	free(ELEMS[i]);
    }
    for (unsigned i = 0; i < NUMRINGBUFS; i++)
    {
	switch (RING_IMPL)
	{
	    case classic :
		p64_ringbuf_free(RINGBUFS[i]);
		break;
	    case lfring :
		p64_lfring_free(RINGBUFS[i]);
		break;
	    case buckring :
		p64_buckring_free(RINGBUFS[i]);
		break;
	    case stack :
		stack_free(RINGBUFS[i]);
		break;
	    case lfstack :
		lfstack_free(RINGBUFS[i]);
		break;
	    case blkring :
		{
		    uint32_t index;
		    void *ev[64];
		    //Free any remaining sentinels
		    p64_blkring_dequeue_nblk(RINGBUFS[i], ev, 64, &index);
		    p64_blkring_free(RINGBUFS[i]);
		    break;
		}
	    case msqueue :
		msqueue_free(RINGBUFS[i]);
		break;
	}
    }
    if (HPD)
    {
	p64_hazptr_unregister();
	p64_hazptr_free(HPD);
    }

    return 0;
}
