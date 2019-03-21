//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _P64_QSBR_H
#define _P64_QSBR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct p64_qsbr p64_qsbr_t;

//Allocate a QSBR data structure where each thread will be able to have up to
//'nelems' objects waiting for reclamation
p64_qsbr_t *p64_qsbr_alloc(uint32_t nelems);

//Free a QSBR data structure
void p64_qsbr_free(p64_qsbr_t *qsbr);

//Register a thread, allocate per-thread resources
void p64_qsbr_register(p64_qsbr_t *qsbr);

//Unregister a thread, free any per-thread resources
void p64_qsbr_unregister(void);

//Signal QSBR data structure that this thread is now acquiring references to
//shared objects
void p64_qsbr_acquire(void);

//Signal QSBR data structure that this thread has released all references
//to shared objects
void p64_qsbr_release(void);

//Signal QSBR data structure that this thread has released all previous
//references to shared objects but will continue to acquire new references
//to shared objects
//This is functionally equivalent to p64_qsbr_release + p64_qsbr_acquire
void p64_qsbr_quiescent(void);

//Retire a removed shared object
//Call 'callback' when object is no longer referenced and can be destroyed
//Return true if object could be retired, false otherwise (no space remaining)
bool p64_qsbr_retire(void *ptr, void (*callback)(void *ptr));

//Force garbage reclamation
//Return number of remaining unreclamined objects
uint32_t p64_qsbr_reclaim(void);

#ifdef __cplusplus
}
#endif

#endif
