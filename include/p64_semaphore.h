//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

//Fair counting semaphore

#ifndef P64_SEMAPHORE_H
#define P64_SEMAPHORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef union
{
    uint64_t a_r;
    struct
    {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	uint32_t rel;
	uint32_t acq;
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#error
#endif
    };
} p64_semaphore_t;

//Initialise a semaphore with the specified count
void p64_sem_init(p64_semaphore_t *sem, uint32_t count);

//Wait for semaphore
void p64_sem_acquire(p64_semaphore_t *sem);
void p64_sem_acquire_n(p64_semaphore_t *sem, uint32_t n);

//Signal semaphore
void p64_sem_release(p64_semaphore_t *sem);
void p64_sem_release_n(p64_semaphore_t *sem, uint32_t n);

#ifdef __cplusplus
}
#endif

#endif
