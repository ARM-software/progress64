//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _P64_HAZARDPTR_H
#define _P64_HAZARDPTR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define HP_REFS_PER_THREAD 3

typedef void **p64_hazardptr_t;
#define P64_HAZARDPTR_NULL NULL

//Acquire a reference to the object which '*pptr' points to
//Return a pointer to the object or NULL (*pptr was NULL)
//Re-use the specified hazard pointer (if *hp != P64_HAZARDPTR_NULL)
//Write any allocated hazard pointer to *hp
//Note that a hazard pointer may have been allocated even if NULL is returned
//hp_acquire() has acquire memory ordering
void *hp_acquire(void **pptr, p64_hazardptr_t *hp);
#ifndef NDEBUG
#define hp_acquire(_a, _b) \
({ \
    p64_hazardptr_t *_c = (_b); \
    void *_d = hp_acquire((_a), _c); \
    if (*(_c) != P64_HAZARDPTR_NULL) \
	hp_annotate(*(_c), __FILE__, __LINE__); \
    _d; \
})
#endif

//Release the reference, updates may have been made
//hp_release() has release memory ordering
void hp_release(p64_hazardptr_t *hp);

//Release the reference, no updates have been made
//hp_release_ro() has release memory ordering for loads only
void hp_release_ro(p64_hazardptr_t *hp);

//p64_hazardptr_t hp = P64_HAZARDPTR_NULL;
//if ((ptr = hp_acquire(&loc, &hp)) != NULL)
//{
//    //Access *ptr
//}
//hp_release(&hp);
////Else loc == NULL

//Retire a removed object
//Call 'callback' when object is no longer referenced and can be destroyed
void hp_retire(void *ptr, void (*callback)(void *ptr));

//Force a (premature) garbage collection
bool hp_gc(void);

//Debugging support
//Annotate a hazard pointer (if != P64_HAZARDPTR_NULL) with file & line
void hp_annotate(p64_hazardptr_t hp, const char *file, unsigned line);

//Print allocated hazard pointers and associated file & line
//Return number of allocated hazard pointers
unsigned hp_dump(FILE *fp);

#ifdef __cplusplus
}
#endif

#endif
