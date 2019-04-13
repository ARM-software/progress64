//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include "p64_semaphore.h"
#include "build_config.h"

#include "arch.h"

#define ACQ_ONE    (1UL << 32)
#define TO_ACQ(x)  (uint32_t)((x) >> 32)
#define TO_REL(x)  (uint32_t)((x)      )

void
p64_sem_init(p64_semaphore_t *sem, uint32_t count)
{
    sem->acq = 0;
    sem->rel = count;
}

static inline void
wait_until_gteq32(uint32_t *loc, uint32_t a)
{
    SEVL();
    while(WFE() && (int32_t)(LDXR32(loc, __ATOMIC_ACQUIRE) - a) < 0)
    {
	DOZE();
    }
}

void inline
p64_sem_acquire_n(p64_semaphore_t *sem, uint32_t n)
{
    uint64_t a_r = __atomic_fetch_add(&sem->a_r, n * ACQ_ONE, __ATOMIC_ACQUIRE);
    uint32_t acq = TO_ACQ(a_r);
    uint32_t rel = TO_REL(a_r);
    if ((int32_t)(rel - (acq + n)) < 0)
    {
	wait_until_gteq32(&sem->rel, acq + n);
    }
}

void
p64_sem_acquire(p64_semaphore_t *sem)
{
    p64_sem_acquire_n(sem, 1);
}

void inline
p64_sem_release_n(p64_semaphore_t *sem, uint32_t n)
{
    __atomic_fetch_add(&sem->rel, n, __ATOMIC_RELEASE);
}

void
p64_sem_release(p64_semaphore_t *sem)
{
    p64_sem_release_n(sem, 1);
}
