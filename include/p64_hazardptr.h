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
void *hp_acquire(void **pptr, p64_hazardptr_t *hp);
void *hp_acquire_fileline(void **pptr, p64_hazardptr_t *hp,
			  const char *file, unsigned line);
#ifndef NDEBUG
#define hp_acquire(_a, _b) hp_acquire_fileline((_a), (_b), __FILE__, __LINE__)
#endif

//Release the reference, updates may have been made
void hp_release(p64_hazardptr_t *hp);
//Release the reference, no updates have been made
void hp_release_ro(p64_hazardptr_t *hp);

//p64_hazardptr_t hp = P64_HAZARDPTR_NULL;
//if ((ptr = hp_acquire(&loc, &hp)) != NULL)
//{
//    //Access *ptr
//    hp_release(&hp);
//}
////Else loc == NULL

//Retire a removed object
//Call 'callback' when object is no longer referenced and can be destroyed
void hp_retire(void *ptr, void (*callback)(void *ptr));

//Force a (premature) garbage collection
bool hp_gc(void);

//For debugging
//Set file & line associated with a hazard pointer (if != P64_HAZARDPTR_NULL)
void hp_set_fileline(p64_hazardptr_t hp, const char *file, unsigned line);
//Print file & line associated with allocated hazard pointers
//Return number of allocated hazard pointers
unsigned hp_print_fileline(FILE *fp);

#ifdef __cplusplus
}
#endif

#endif
