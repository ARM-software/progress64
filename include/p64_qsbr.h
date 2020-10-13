//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

//Safe memory reclamation using quiescent state based reclamation

#ifndef P64_QSBR_H
#define P64_QSBR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct p64_qsbrdomain p64_qsbrdomain_t;

//Allocate a QSBR domain where each thread will be able to have up to
//'maxobjs' retired objects waiting for reclamation (0 < maxobjs <= 0x80000000).
//An unlimited number of objects will be safe from premature reclamation
p64_qsbrdomain_t *p64_qsbr_alloc(uint32_t maxobjs);

//Free a QSBR domain
void p64_qsbr_free(p64_qsbrdomain_t *qsbr);

//Register and activate a thread, allocate per-thread resources
void p64_qsbr_register(p64_qsbrdomain_t *qsbr);

//Deactivate and unregister a thread, free any per-thread resources
void p64_qsbr_unregister(void);

//Reactivate an inactive thread, it is again acquiring references to shared
//objects
void p64_qsbr_reactivate(void);

//Deactive an active thread, any references to shared objects have been released
//and the thread will not acquire any new references
//p64_qsbr_deactivate() should called e.g. when the thread is going to block for
//a long or indeterminate time during which we do not want to prevent
//reclamation
void p64_qsbr_deactivate(void);

//Signal QSBR domain that this thread has released all previous
//references to shared objects
//p64_qsbr_quiescent() is expected to be called from some application main loop
void p64_qsbr_quiescent(void);

//p64_qsbr_acquire() and p64_qsbr_release() allow for use of QSBR in e.g.
//libraries. When the number of calls to p64_qsbr_release() matches the
//number of calls to p64_qsbr_acquire(), p64_qsbr_quiescent() is called.
void p64_qsbr_acquire(void);
void p64_qsbr_release(void);

//Retire a removed shared object
//Call 'callback' when object is no longer referenced and can be destroyed
//Return true if object could be retired, false otherwise (no space remaining)
bool p64_qsbr_retire(void *ptr, void (*callback)(void *ptr));

//Force garbage reclamation
//Return number of remaining unreclaimed objects
uint32_t p64_qsbr_reclaim(void);

#ifdef __cplusplus
}
#endif

#endif
