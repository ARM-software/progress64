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

#include "p64_qsbr.h"
#include "p64_hashtable.h"
#include "p64_hopscotch.h"
#include "p64_cuckooht.h"
#include "build_config.h"
#include "common.h"
#include "arch.h"

#define container_of(pointer, type, member) \
    ((type *)(void *)(((char *)pointer) - offsetof(type, member)))

//Thread priority and scheduling
#define PRIO 1
#define SCHED SCHED_FIFO

#define MAXVECSIZE 32

enum operation { Insert, Remove, LookupHit, LookupMiss, TheBeyond };

struct object
{
    union
    {
	p64_hashelem_t he;
	p64_cuckooelem_t ce;
    };
    uint32_t key;
} ALIGNED(CACHE_LINE);

static void *HT = NULL;
static enum operation OPER;
static p64_qsbrdomain_t *QSBR;
static pthread_t tid[MAXTHREADS];
static uint32_t NUMTHREADS = 2;
static int cpus[MAXTHREADS];
static unsigned long CPUFREQ;
static unsigned long AFFINITY = ~0UL;
static uint32_t NUMKEYS = 10000000;
static uint32_t VECSIZE = 0;
static struct object *OBJS;//Array of all objects
static uint64_t THREAD_BARRIER ALIGNED(CACHE_LINE);
static bool HOPSCOTCH = false;
static bool CUCKOOHT = false;
static bool VERBOSE = false;
static sem_t ALL_DONE ALIGNED(CACHE_LINE);
static struct timespec END_TIME;

//Wait for my signal to begin
static void
barrier_thr_begin(uint32_t idx)
{
    uint64_t thrmask = 1UL << idx;
    while ((LDX(&THREAD_BARRIER, __ATOMIC_ACQUIRE) & thrmask) == 0)
    {
	WFE();
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
    uint64_t thrmask = numthreads < 64 ? (1UL << numthreads) - 1 : ~0UL;
    __atomic_store_n(&THREAD_BARRIER, thrmask, __ATOMIC_RELEASE);
    sem_wait(&ALL_DONE);
}

//Wait until all threads are done
static void
barrier_all_wait(uint32_t numthreads)
{
    (void)numthreads;
    while (LDX(&THREAD_BARRIER, __ATOMIC_ACQUIRE) != 0)
    {
	WFE();
    }
}

#if defined __aarch64__ && defined __ARM_FEATURE_CRC32
#include <arm_acle.h>
#define CRC32C(x, y) __crc32cw((x), (y))
#elif defined __x86_64__ && defined __SSE4_2__
#include <x86intrin.h>
//x86 crc32 intrinsics seem to compute CRC32C (not CRC32)
#define CRC32C(x, y) __crc32d((x), (y))
#else
//E.g. ARMv7 does not have a CRC instruction
//Use a pseudo RNG instead
static inline uint32_t
xorshift32(uint32_t x)
{
    /* Algorithm "xor" from p. 4 of Marsaglia, "Xorshift RNGs" */
    //x == 0 will return 0
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}
#define CRC32C(x, y) xorshift32((y))
#endif

static inline uint64_t
compute_hash(uint32_t key)
{
    return (uint64_t)CRC32C(0, key);
}

static void
thr_insert(uint32_t tidx)
{
    for (uint32_t idx = tidx; idx < NUMKEYS; idx += NUMTHREADS)
    {
	struct object *obj = &OBJS[idx];
	uint32_t key = idx;//Keys are unique
	assert(obj->key == key);
	uintptr_t hash = compute_hash(key);//Hashes may not be unique
	bool success;
	if (HOPSCOTCH)
	{
	    success = p64_hopscotch_insert(HT, obj, hash);
	}
	else if (CUCKOOHT)
	{
	    success = p64_cuckooht_insert(HT, &obj->ce, hash);
	}
	else
	{
	    p64_hashtable_insert(HT, &obj->he, hash);
	    success = true;
	}
	if (!success)
	{
	    fprintf(stderr, "Failed to insert key %u (hash=%lx)\n", key, hash);
	    if (HOPSCOTCH)
	    {
		void p64_hopscotch_check(p64_hopscotch_t *ht);
		p64_hopscotch_check(HT);
	    }
	    else if (CUCKOOHT)
	    {
		void p64_cuckooht_check(p64_cuckooht_t *ht);
		p64_cuckooht_check(HT);
	    }
	    exit(EXIT_FAILURE);
	}
    }
}

static void
thr_remove(uint32_t tidx)
{
    for (uint32_t idx = tidx; idx < NUMKEYS; idx += NUMTHREADS)
    {
	struct object *obj = &OBJS[idx];
	uint32_t key = idx;//Keys are unique
	assert(obj->key == key);
	uintptr_t hash = compute_hash(key);//Hashes may not be unique
	bool success;
	if (HOPSCOTCH)
	{
	    success = p64_hopscotch_remove(HT, obj, hash);
	}
	else if (CUCKOOHT)
	{
	    success = p64_cuckooht_remove(HT, &obj->ce, hash);
	}
	else
	{
	    success = p64_hashtable_remove(HT, &obj->he, hash);
	}
	if (!success)
	{
	    fprintf(stderr, "Failed to remove key %u\n", key);
	    exit(EXIT_FAILURE);
	}
    }
}

static void
thr_lookup_hit(uint32_t tidx)
{
    uint32_t vecsize = VECSIZE;
    uint32_t numthreads = NUMTHREADS;
    uint32_t numkeys = NUMKEYS;
    for (uint32_t i = 0; i < numthreads; i++)
    {
	if (vecsize != 0)
	{
	    uint32_t k[MAXVECSIZE];
	    const void *keys[MAXVECSIZE];
	    uintptr_t hashes[MAXVECSIZE];
	    void *res[MAXVECSIZE];
	    for (uint32_t j = 0; j < vecsize; j++)
	    {
		keys[j] = &k[j];
	    }

	    for (uint32_t i = tidx;
		 i + vecsize <= numkeys;
		 i += numthreads * vecsize)
	    {
		for (uint32_t j = 0; j < vecsize; j++)
		{
		    k[j] = i + j; //Keys are unique
		    hashes[j] = compute_hash(k[j]);//Hashes may not be unique
		}
		if (HOPSCOTCH)
		{
		    p64_hopscotch_lookup_vec(HT, vecsize, keys, hashes, res);
		}
		else if (CUCKOOHT)
		{
		    p64_cuckooht_lookup_vec(HT, vecsize, keys, hashes, (void*)res);
		}
		else
		{
		    p64_hashtable_lookup_vec(HT, vecsize, keys, hashes, (void*)res);
		}
		for (uint32_t j = 0; j < vecsize; j++)
		{
		    if (res[j] == NULL)
		    {
			fprintf(stderr, "Lookup failed to find key %u\n",
				k[j]);
			exit(EXIT_FAILURE);
		    }
		    struct object *obj;
		    if (HOPSCOTCH)
		    {
			obj = res[j];
		    }
		    else if (CUCKOOHT)
		    {
			obj = container_of(res[j], struct object, ce);
		    }
		    else
		    {
			obj = container_of(res[j], struct object, he);
		    }
		    if (obj->key != k[j])
		    {
			fprintf(stderr, "Lookup returned wrong key: "
				"wanted %u, actual %u\n",
				k[j], obj->key);
			exit(EXIT_FAILURE);
		    }
		}
	    }
	}
	else
	{
	    for (uint32_t idx = tidx; idx < numkeys; idx += numthreads)
	    {
		struct object *obj = NULL;
		uint32_t key = idx;//Keys are unique
		uintptr_t hash = compute_hash(key);//Hashes may not be unique
		if (HOPSCOTCH)
		{
		    //UBSAN complains if &hp is not specified
		    p64_hazardptr_t hp = P64_HAZARDPTR_NULL;
		    obj = p64_hopscotch_lookup(HT, &key, hash, &hp);
		}
		else if (CUCKOOHT)
		{
		    p64_cuckooelem_t *ce =
			p64_cuckooht_lookup(HT, &key, hash, NULL);
		    if (ce != NULL)
		    {
			obj = container_of(ce, struct object, ce);
		    }
		}
		else
		{
		    //UBSAN complains if &hp is not specified
		    p64_hazardptr_t hp = P64_HAZARDPTR_NULL;
		    p64_hashelem_t *he =
			p64_hashtable_lookup(HT, &key, hash, &hp);
		    if (he != NULL)
		    {
			obj = container_of(he, struct object, he);
		    }
		}
		if (obj == NULL)
		{
		    fprintf(stderr, "Lookup failed to find key %u\n", key);
		    exit(EXIT_FAILURE);
		}
		else if (obj->key != key)
		{
		    fprintf(stderr, "Lookup returned wrong key: "
			    "wanted %u, actual %u\n",
			    key, obj->key);
		    exit(EXIT_FAILURE);
		}
	    }
	}
    }
}

static void
thr_lookup_miss(uint32_t tidx)
{
    uint32_t vecsize = VECSIZE;
    uint32_t numthreads = NUMTHREADS;
    uint32_t numkeys = NUMKEYS;
    for (uint32_t i = 0; i < numthreads; i++)
    {
	if (vecsize != 0)
	{
	    uint32_t k[MAXVECSIZE];
	    const void *keys[MAXVECSIZE];
	    uintptr_t hashes[MAXVECSIZE];
	    void *res[MAXVECSIZE];
	    for (uint32_t j = 0; j < vecsize; j++)
	    {
		keys[j] = &k[j];
	    }

	    for (uint32_t i = tidx;
		 i + vecsize <= numkeys;
		 i += numthreads * vecsize)
	    {
		for (uint32_t j = 0; j < vecsize; j++)
		{
		    k[j] = numkeys + i + j;
		    hashes[j] = compute_hash(k[j]);//Hashes may not be unique
		}
		if (HOPSCOTCH)
		{
		    p64_hopscotch_lookup_vec(HT, vecsize, keys, hashes, res);
		}
		else if (CUCKOOHT)
		{
		    p64_cuckooht_lookup_vec(HT, vecsize, keys, hashes, (void*)res);
		}
		else
		{
		    p64_hashtable_lookup_vec(HT, vecsize, keys, hashes, (void*)res);
		}
		for (uint32_t j = 0; j < vecsize; j++)
		{
		    if (res[j] != NULL)
		    {
			fprintf(stderr, "Lookup non-existent key %u found something\n", k[j]);
			exit(EXIT_FAILURE);
		    }
		}
	    }
	}
	else
	{
	    for (uint32_t idx = tidx; idx < numkeys; idx += numthreads)
	    {
		struct object *obj = NULL;
		uint32_t key = numkeys + idx;
		uintptr_t hash = compute_hash(key);//Hashes may not be unique
		if (HOPSCOTCH)
		{
		    //UBSAN complains if &hp is not specified
		    p64_hazardptr_t hp = P64_HAZARDPTR_NULL;
		    obj = p64_hopscotch_lookup(HT, &key, hash, &hp);
		}
		else if (CUCKOOHT)
		{
		    p64_cuckooelem_t *ce =
			p64_cuckooht_lookup(HT, &key, hash, NULL);
		    if (ce != NULL)
		    {
			obj = container_of(ce, struct object, ce);
		    }
		}
		else
		{
		    //UBSAN complains if &hp is not specified
		    p64_hazardptr_t hp = P64_HAZARDPTR_NULL;
		    p64_hashelem_t *he =
			p64_hashtable_lookup(HT, &key, hash, &hp);
		    if (he != NULL)
		    {
			obj = container_of(he, struct object, he);
		    }
		}
		if (obj != NULL)
		{
		    fprintf(stderr, "Lookup non-existent key %u found key %u\n",
			    key, obj->key);
		    exit(EXIT_FAILURE);
		}
	    }
	}
    }
}

static void
thr_execute(uint32_t tidx)
{
    if (OPER == Insert)
    {
	thr_insert(tidx);
    }
    else if (OPER == Remove)
    {
	thr_remove(tidx);
    }
    else if (OPER == LookupHit)
    {
	thr_lookup_hit(tidx);
    }
    else//OPER == LookupMiss
    {
	thr_lookup_miss(tidx);
    }
}

static void *
entrypoint(void *arg)
{
    unsigned tidx = (uintptr_t)arg;

    p64_qsbr_register(QSBR);

    //Iterate over operations
    for (uint32_t i = Insert; i < TheBeyond; i++)
    {
	//Wait for my signal to start
	barrier_thr_begin(tidx);

	thr_execute(tidx);

	//Signal I am done
	barrier_thr_done(tidx);
    }

    p64_qsbr_unregister();

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
benchmark(uint32_t numthreads, enum operation oper)
{
    struct timespec ts;

    OPER = oper;

    //Re-initialise semaphore
    int res = sem_init(&ALL_DONE, 0, 0);
    if (res < 0)
    {
	perror("sem_init"), exit(EXIT_FAILURE);
    }

    //Read starting time
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t start = ts.tv_sec * 1000000000ULL + ts.tv_nsec;
    //Start worker threads
    barrier_all_begin(numthreads);
    //Wait for worker threads to complete
    barrier_all_wait(numthreads);
    //Read end time
    ts = END_TIME;

    uint32_t numops = NUMKEYS;
    if (oper == LookupHit || oper == LookupMiss)
    {
	numops *= numthreads;
    }
    uint64_t elapsed_ns = ts.tv_sec * 1000000000ULL + ts.tv_nsec - start;
    uint32_t elapsed_s = elapsed_ns / 1000000000ULL;
    printf("%u %s, %u.%04llu seconds, ",
	    numops,
	    oper == Insert ? "insertions" : oper == Remove ? "removals" : oper == LookupHit ? "lookup hits" : "lookup misses",
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

static int
compare_ht_key(const p64_hashelem_t *he, const void *key)
{
    const struct object *obj = container_of(he, struct object, he);
    return obj->key - *(const uint32_t *)key;
}

static int
compare_hs_key(const void *he, const void *key)
{
    const struct object *obj = he;
    return obj->key - *(const uint32_t *)key;
}

static int
compare_cc_key(const p64_cuckooelem_t *ce, const void *key)
{
    const struct object *obj = container_of(ce, struct object, ce);
    return obj->key - *(const uint32_t *)key;
}

int
main(int argc, char *argv[])
{
    int c;
    uint32_t numelems = NUMKEYS;
    uint32_t numcells = 0;

    while ((c = getopt(argc, argv, "a:c:Cf:Hk:m:s:t:v:V")) != -1)
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
	    case 'm' :
		{
		    int ne = atoi(optarg);
		    if (ne < 1)
		    {
			fprintf(stderr, "Invalid number of elements %d\n", ne);
			exit(EXIT_FAILURE);
		    }
		    numelems = (unsigned)ne;
		    break;
		}
	    case 'c' :
		{
		    int nc = atoi(optarg);
		    if (nc < 0)
		    {
			fprintf(stderr, "Invalid number of cells %d\n", nc);
			exit(EXIT_FAILURE);
		    }
		    numcells = (unsigned)nc;
		    break;
		}
	    case 'C' :
		CUCKOOHT = true;
		break;
	    case 'f' :
		{
		    CPUFREQ = atol(optarg);
		    break;
		}
	    case 'H' :
		HOPSCOTCH = true;
		break;
	    case 'k' :
		{
		    int nk = atoi(optarg);
		    if (nk < 1)
		    {
			fprintf(stderr, "Invalid number of keys %d\n", nk);
			exit(EXIT_FAILURE);
		    }
		    NUMKEYS = (unsigned)nk;
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
	    {
		int32_t vz = atoi(optarg);
		if (vz < 1 || (uint32_t)vz > MAXVECSIZE)
		{
		    fprintf(stderr, "Invalid vector size %s\n", optarg);
		    exit(EXIT_FAILURE);
		}
		VECSIZE = vz;
		break;
	    }
	    case 'V' :
		VERBOSE = true;
		break;
	    default :
usage :
		fprintf(stderr, "Usage: bm_hashtab <options>\n"
			"-a <binmask>     CPU affinity mask (default base 2)\n"
			"-c <size>        Size of cellar\n"
			"-C               Use cuckoo hash table\n"
			"-f <cpufreq>     CPU frequency in kHz\n"
			"-H               Use hopscotch hash table\n"
			"-k <numkeys>     Number of keys\n"
			"-m <size>        Size of main hash table\n"
			"-t <numthr>      Number of threads\n"
			"-v <vecsize>     Use vector lookup\n"
			"-V               Verbose\n"
		       );
		exit(EXIT_FAILURE);
	}
    }
    if (optind != argc)
    {
	goto usage;
    }

    printf("%s: main size %u, cellar size %u, %u keys, "
	   "%u thread%s, affinity mask=0x%lx\n",
	    HOPSCOTCH ? "hopscotch" : CUCKOOHT ? "cuckooht" : "michaelht",
	    numelems,
	    numcells,
	    NUMKEYS,
	    NUMTHREADS, NUMTHREADS != 1 ? "s" : "",
	    AFFINITY);

    OBJS = aligned_alloc(CACHE_LINE, NUMKEYS * sizeof(struct object));
    if (OBJS == NULL)
    {
	perror("malloc"), exit(EXIT_FAILURE);
    }
    for (uint32_t i = 0; i < NUMKEYS; i++)
    {
	//Keys are unique
	OBJS[i].key = i;
    }

    QSBR = p64_qsbr_alloc(10);
    if (QSBR == NULL)
    {
	perror("p64_qsbr_alloc"), abort();
    }

    if (HOPSCOTCH)
    {
	HT = p64_hopscotch_alloc(numelems, numcells, compare_hs_key, 0);
	if (HT == NULL)
	    perror("p64_hopscotch_alloc"), abort();
    }
    else if (CUCKOOHT)
    {
	HT = p64_cuckooht_alloc(numelems, numcells, compare_cc_key, 0);
	if (HT == NULL)
	    perror("p64_cuckooht_alloc"), abort();
    }
    else
    {
	HT = p64_hashtable_alloc(numelems, compare_ht_key, 0);
	if (HT == NULL)
	    perror("p64_hashtable_alloc"), abort();
    }

    if (NUMTHREADS != 0)
    {
	create_threads(NUMTHREADS, AFFINITY);
	benchmark(NUMTHREADS, Insert);
	benchmark(NUMTHREADS, LookupHit);
	benchmark(NUMTHREADS, LookupMiss);
	if (VERBOSE)
	{
	    if (HOPSCOTCH)
	    {
		void p64_hopscotch_check(p64_hopscotch_t *ht);
		p64_hopscotch_check(HT);
	    }
	    else if (CUCKOOHT)
	    {
		void p64_cuckooht_check(p64_cuckooht_t *ht);
		p64_cuckooht_check(HT);
	    }
	}
	benchmark(NUMTHREADS, Remove);
    }

    //Clean up
    for (uint32_t thr = 0; thr < NUMTHREADS; thr++)
    {
	(void)pthread_cancel(tid[thr]);
    }
    (void)sem_destroy(&ALL_DONE);
    if (HOPSCOTCH)
    {
	p64_hopscotch_free(HT);
    }
    else if (CUCKOOHT)
    {
	p64_cuckooht_free(HT);
    }
    else
    {
	p64_hashtable_free(HT);
    }

    p64_qsbr_free(QSBR);

    free(OBJS);
    return 0;
}
