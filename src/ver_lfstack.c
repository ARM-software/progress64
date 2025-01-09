//Copyright (c) 2025, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "p64_lfstack.h"
#include "os_abstraction.h"

#include "verify.h"

#define NUMTHREADS 2

static p64_lfstack_t lfs_stk;
static p64_lfstack_elem_t *lfs_elems[NUMTHREADS];

static void
ver_lfstack_init(uint32_t numthreads)
{
    if (numthreads != NUMTHREADS)
    {
	abort();
    }
    p64_lfstack_init(&lfs_stk);
}

static void
ver_lfstack_fini(uint32_t numthreads)
{
    (void)numthreads;
    //VERIFY_ASSERT(lfs_elems[0] == NULL && lfs_elems[1] == NULL);
}

static void
ver_lfstack_exec(uint32_t id)
{
    p64_lfstack_elem_t *elem = p64_malloc(sizeof(p64_lfstack_elem_t), 64);
    VERIFY_ASSERT(elem != NULL);
    lfs_elems[id] = elem;
    p64_lfstack_enqueue(&lfs_stk, elem);
    elem = p64_lfstack_dequeue(&lfs_stk);
    VERIFY_ASSERT(elem != NULL);
    VERIFY_ASSERT(elem == lfs_elems[0] || elem == lfs_elems[1]);
    if (elem == lfs_elems[0])
    {
	lfs_elems[0] = NULL;
    }
    else
    {
	lfs_elems[1] = NULL;
    }
    p64_mfree(elem);
}

struct ver_funcs ver_lfstack =
{
    "lfstack", ver_lfstack_init, ver_lfstack_exec, ver_lfstack_fini
};
