//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _P64_HAZARDPTR_H
#define _P64_HAZARDPTR_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef void **p64_hazardptr_t;
#define P64_HAZARDPTR_NULL NULL

typedef struct p64_hpdomain p64_hpdomain_t;

//Allocate a hazard pointer domain where each thread will be able to keep up
//to 'nrefs' objects safe from premature reclamation
p64_hpdomain_t *p64_hazptr_alloc(uint32_t nrefs);

//Free a hazard pointer domain
void p64_hazptr_free(p64_hpdomain_t *hdom);

//Register a thread, allocate per-thread resources
void p64_hazptr_register(p64_hpdomain_t *hdom);

//Unregister a thread, free any per-thread resources
void p64_hazptr_unregister(void);

//Acquire a reference to the object which '*pptr' points to
//Return a pointer to the object or NULL (*pptr was NULL)
//Re-use the specified hazard pointer (if *hp != P64_HAZARDPTR_NULL)
//Write any allocated hazard pointer to *hp
//Note that a hazard pointer may have been allocated even if NULL is returned
//p64_hazptr_acquire() has acquire memory ordering
void *p64_hazptr_acquire(void **pptr, p64_hazardptr_t *hp);
#ifndef NDEBUG
#define p64_hazptr_acquire(_a, _b) \
({ \
    p64_hazardptr_t *_c = (_b); \
    void *_d = p64_hazptr_acquire((_a), _c); \
    if (*(_c) != P64_HAZARDPTR_NULL) \
	p64_hazptr_annotate(*(_c), __FILE__, __LINE__); \
    _d; \
})
#endif

//Release the reference, updates may have been made
//p64_hazptr_release() has release memory ordering
void p64_hazptr_release(p64_hazardptr_t *hp);

//Release the reference, no updates have been made
//p64_hazptr_release_ro() has release memory ordering for loads only
void p64_hazptr_release_ro(p64_hazardptr_t *hp);

//p64_hazardptr_t hp = P64_HAZARDPTR_NULL;
//if ((ptr = p64_hazptr_acquire(&loc, &hp)) != NULL)
//{
//    //Access *ptr
//}
//p64_hazptr_release(&hp);
////Else loc == NULL

//Retire a removed object
//Call 'callback' when object is no longer referenced and can be destroyed
void p64_hazptr_retire(void *ptr, void (*callback)(void *ptr));

//Force garbage reclamation
//Return number of remaining unreclamined objects
uint32_t p64_hazptr_reclaim(void);

//Debugging support
//Annotate a hazard pointer (if != P64_HAZARDPTR_NULL) with file & line
void p64_hazptr_annotate(p64_hazardptr_t hp, const char *file, unsigned line);

//Print allocated hazard pointers and associated file & line
//Return number of allocated hazard pointers
uint32_t p64_hazptr_dump(FILE *fp);

#ifdef __cplusplus
}
#endif

#endif
