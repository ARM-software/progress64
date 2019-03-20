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

//Allocate a QSBR data structure where each thread will have space for
//'nelems' unreclaimed objects
p64_qsbr_t *p64_qsbr_alloc(uint32_t nelems);

//Clean up any per-thread resources
void p64_qsbr_cleanup(void);

//Free a QSBR data structure
void p64_qsbr_free(p64_qsbr_t *qsbr);

//Signal QSBR data structure that this thread is acquiring references to
//shared objects
//This call also allocates any necessary per-thread resources
void p64_qsbr_acquire(p64_qsbr_t *qsbr);

//Signal QSBR data structure that this thread has released all references
//to shared objects
void p64_qsbr_release(void);

//Signal QSBR data structure that this thread has released all previous
//references to shared objects but will continue to acquire new references
//to shared objects
//This is essentially equivalent to p64_qsbr_release + p64_qsbr_acquire
void p64_qsbr_quiescent(void);

//Retire a removed object
//Call 'callback' when object is no longer referenced and can be destroyed
//Return true if objects could be retired, false otherwise (no space remaining)
bool p64_qsbr_retire(void *ptr, void (*callback)(void *ptr));

//Force garbage reclamation
//Return number of remaining unreclamined objects
uint32_t p64_qsbr_reclaim(void);

#ifdef __cplusplus
}
#endif

#endif
