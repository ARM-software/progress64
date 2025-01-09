//Copyright (c) 2025, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "p64_buckring.h"
#include "p64_errhnd.h"
#include "atomic.h"

#include "verify.h"

#define NUMTHREADS 2

static p64_buckring_t *buckr_rb;
static uint32_t *buckr_elems[3];
static uint32_t elem_mask;

static void
ver_buckring2_init(uint32_t numthreads)
{
    if (numthreads != NUMTHREADS)
    {
	abort();
    }
    buckr_rb = p64_buckring_alloc(64, 0);
    VERIFY_ASSERT(buckr_rb != NULL);
    uint32_t *elem0 = malloc(sizeof(uint32_t));
    uint32_t *elem1 = malloc(sizeof(uint32_t));
    uint32_t *elem2 = malloc(sizeof(uint32_t));
    VERIFY_ASSERT(elem0 != NULL);
    VERIFY_ASSERT(elem1 != NULL);
    VERIFY_ASSERT(elem2 != NULL);
    *elem0 = 0;
    *elem1 = 1;
    *elem2 = 2;
    buckr_elems[0] = elem0;
    buckr_elems[1] = elem1;
    buckr_elems[2] = elem2;
    elem_mask = 0;
}

static int
error_handler(const char *module, const char *cur_err, uintptr_t val)
{
    (void)module;
    (void)cur_err;
    (void)val;
    VERIFY_ERROR(cur_err);
    return P64_ERRHND_RETURN;
}

static void
ver_buckring2_fini(uint32_t numthreads)
{
    (void)numthreads;
    VERIFY_ASSERT(elem_mask == 7);
    free(buckr_elems[2]);
    free(buckr_elems[1]);
    free(buckr_elems[0]);
    p64_errhnd_install(error_handler);
    p64_buckring_free(buckr_rb);
}

static void
ver_buckring2_exec(uint32_t id)
{
    if (id == 0)
    {
	uint32_t idx;
	uint32_t *elems[2];
	VERIFY_ASSERT(p64_buckring_enqueue(buckr_rb, (void **)&buckr_elems[0], 2) == 2);
	elems[0] = NULL;
	elems[1] = NULL;
	for (;;)
	{
	    uint32_t r = p64_buckring_dequeue(buckr_rb, (void **)elems, 2, &idx);
	    VERIFY_ASSERT(r == 0 || r == 2);
	    if (r == 2)
	    {
		break;
	    }
	    VERIFY_YIELD();
	}
	//printf("0: idx=%u\n", idx);
	//printf("0: elems[0]=%p\n", elems[0]);
	//printf("0: elems[1]=%p\n", elems[1]);
	VERIFY_ASSERT(idx == 0 || idx == 1);
	VERIFY_ASSERT(elems[0] != elems[1]);
	for (int i = 0; i < 2; i++)
	{
	    for (int j = 0; j < 3; j++)
	    {
		if (elems[i] == buckr_elems[j])
		{
		    elem_mask ^= 1U << j;
		    break;
		}
	    }
	}
    }
    else //id == 1
    {
	uint32_t idx;
	VERIFY_ASSERT(p64_buckring_enqueue(buckr_rb, (void **)&buckr_elems[2], 1) == 1);
	//We cannot successfully dequeue element until all preceding enqueue's have completed
	uint32_t *elem = NULL;
	while (p64_buckring_dequeue(buckr_rb, (void **)&elem, 1, &idx) == 0)
	{
	    VERIFY_YIELD();
	}
	//printf("1: idx=%u\n", idx);
	//printf("1: elem=%p\n", elem);
	VERIFY_ASSERT(idx == 0 || idx == 2);
	for (int j = 0; j < 3; j++)
	{
	    if (elem == buckr_elems[j])
	    {
		elem_mask ^= 1U << j;
		break;
	    }
	}
	//Enqueue: 012 201
	//Dequeue: 01:2 0:12 20:1 2:01
    }
}

struct ver_funcs ver_buckring2 =
{
    "buckring2", ver_buckring2_init, ver_buckring2_exec, ver_buckring2_fini
};
