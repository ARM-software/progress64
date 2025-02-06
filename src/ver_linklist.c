//Copyright (c) 2025, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <arm_acle.h>
#include "p64_linklist.h"
#include "os_abstraction.h"

#include "verify.h"

#define NUMTHREADS 2

#define container_of(pointer, type, member) \
    ((type *)(void *)(((char *)pointer) - offsetof(type, member)))

struct object
{
    _Alignas(64)
    p64_linklist_t elem;
    uint32_t data;
};

static _Alignas(64) p64_linklist_t ll_list;
static struct object ll_elems[NUMTHREADS];

static uint32_t
compare_data(const void *data, const p64_linklist_t *elem)
{
    const struct object *obj = container_of(elem, struct object, elem);
    if (obj->data == *(const uint32_t *)data)
    {
	return P64_LINKLIST_F_STOP | P64_LINKLIST_F_RETURN;
    }
    return 0;
}

static p64_linklist_t *
lookup(p64_linklist_t *list, uint32_t data)
{
    return p64_linklist_traverse(list, compare_data, &data);
}

static void
ver_linklist1_init(uint32_t numthreads)
{
    if (numthreads != NUMTHREADS)
    {
	abort();
    }
    p64_linklist_init(&ll_list);
    ll_elems[0].data = 242;
    ll_elems[1].data = 243;
}

static void
ver_linklist1_fini(uint32_t numthreads)
{
    (void)numthreads;
    VERIFY_ASSERT(ll_list.next == NULL);
}

static void
ver_linklist1_exec(uint32_t id)
{
    struct object *my_obj = &ll_elems[id];
    //Insert our element into list
    p64_linklist_insert(&ll_list, &ll_list, &my_obj->elem);
    //Look up our element from data
    p64_linklist_t *elem = lookup(&ll_list, my_obj->data);
    VERIFY_ASSERT(elem == &ll_elems[id].elem);
    //Remove our element from list
    p64_linklist_remove(&ll_list, elem);
    //Verification that our element isn't in list anymore done in fini
}

struct ver_funcs ver_linklist1 =
{
    "linklist1", ver_linklist1_init, ver_linklist1_exec, ver_linklist1_fini
};

static void
ver_linklist2_init(uint32_t numthreads)
{
    if (numthreads != NUMTHREADS)
    {
	abort();
    }
    p64_linklist_init(&ll_list);
    ll_elems[0].data = 242;
    ll_elems[1].data = 243;
}

static void
ver_linklist2_fini(uint32_t numthreads)
{
    (void)numthreads;
    VERIFY_ASSERT(ll_list.next == &ll_elems[1].elem);
    VERIFY_ASSERT(ll_elems[1].elem.next == NULL);

}

static void
ver_linklist2_exec(uint32_t id)
{
    if (id == 0)
    {
	struct object *my_obj0 = &ll_elems[0];
	struct object *my_obj1 = &ll_elems[1];
	p64_linklist_insert(&ll_list, &ll_list, &my_obj0->elem);
	p64_linklist_insert(&ll_list, &my_obj0->elem, &my_obj1->elem);
    }
    else //id == 1
    {
	p64_linklist_t *elem;
	while ((elem = lookup(&ll_list, ll_elems[0].data)) == NULL)
	{
	    VERIFY_YIELD();
	}
	p64_linklist_remove(&ll_list, &ll_elems[0].elem);
    }
}

struct ver_funcs ver_linklist2 =
{
    "linklist2", ver_linklist2_init, ver_linklist2_exec, ver_linklist2_fini
};

static void
ver_linklist3_init(uint32_t numthreads)
{
    if (numthreads != NUMTHREADS)
    {
	abort();
    }
    p64_linklist_init(&ll_list);
    ll_elems[0].data = 242;
    ll_elems[1].data = 243;
}

static void
ver_linklist3_fini(uint32_t numthreads)
{
    (void)numthreads;
    VERIFY_ASSERT(ll_list.next == &ll_elems[1].elem);
    VERIFY_ASSERT(ll_elems[1].elem.next == NULL);

}

//The inverse of ver_linklist2
static void
ver_linklist3_exec(uint32_t id)
{
    if (id == 1)
    {
	struct object *my_obj0 = &ll_elems[0];
	struct object *my_obj1 = &ll_elems[1];
	p64_linklist_insert(&ll_list, &ll_list, &my_obj0->elem);
	p64_linklist_insert(&ll_list, &my_obj0->elem, &my_obj1->elem);
    }
    else //id == 0
    {
	p64_linklist_t *elem;
	while ((elem = lookup(&ll_list, ll_elems[0].data)) == NULL)
	{
	    VERIFY_YIELD();
	}
	p64_linklist_remove(&ll_list, &ll_elems[0].elem);
    }
}

struct ver_funcs ver_linklist3 =
{
    "linklist3", ver_linklist3_init, ver_linklist3_exec, ver_linklist3_fini
};
