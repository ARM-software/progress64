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
static struct object ll_elems[2 * NUMTHREADS];

static p64_linklist_t *
ll_lookup(p64_linklist_t *list, uint32_t data)
{
    for (;;)
    {
	p64_linklist_cursor_t cursor = { list };
	p64_linklist_status_t stat;
	while ((stat = p64_linklist_cursor_next(&cursor)) != p64_ll_predmark)
	{
	    VERIFY_ASSERT(stat == p64_ll_success);
	    if (cursor.curr == NULL)
	    {
		//Element not found, someone else might have removed it
		return NULL;
	    }
	    const struct object *obj = container_of(cursor.curr, struct object, elem);
	    if (obj->data == data)
	    {
		return cursor.curr;
	    }
	}
	//Curr/predecessor removed, can't continue from here, must restart from beginning
    }
}

static void
ll_insert(p64_linklist_t *list, p64_linklist_t *pred, p64_linklist_t *elem)
{
    //First attempt to insert 'elem' after 'pred'
    //If this fails, we insert 'elem' after list head
    for (;;)
    {
	p64_linklist_status_t stat = p64_linklist_insert(pred, elem);
	if (stat == p64_ll_success)
	{
	    return;
	}
	VERIFY_ASSERT(stat == p64_ll_predmark);
	pred = list;
    }
}

static void
ll_remove(p64_linklist_t *list, p64_linklist_t *elem)
{
    for (;;)
    {
	p64_linklist_cursor_t cursor = { list };
	p64_linklist_t *pred = list;
	p64_linklist_status_t stat;
	while ((stat = p64_linklist_cursor_next(&cursor)) != p64_ll_predmark)
	{
	    VERIFY_ASSERT(stat == p64_ll_success);
	    if (cursor.curr == NULL)
	    {
		//Element not found, someone else might have removed it
		return;
	    }
	    else if (cursor.curr == elem)
	    {
		if (p64_linklist_remove(pred, elem) == p64_ll_success)
		{
		    return;
		}
		//Else remove failed
		break;
	    }
	    pred = cursor.curr;
	}
	//Curr/predecessor removed, can't continue from here, must restart from beginning
    }
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
    //ll_lookup(&ll_list, ~0U);
    VERIFY_ASSERT(ll_list.next == NULL);
}

static void
ver_linklist1_exec(uint32_t id)
{
    struct object *my_obj = &ll_elems[id];
    //Insert our element into list
    ll_insert(&ll_list, &ll_list, &my_obj->elem);
    //Look up our element from data
    p64_linklist_t *elem = ll_lookup(&ll_list, my_obj->data);
    VERIFY_ASSERT(elem == &ll_elems[id].elem);
    //Remove our element from list
    ll_remove(&ll_list, elem);
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
	ll_insert(&ll_list, &ll_list, &my_obj0->elem);
	ll_insert(&ll_list, &my_obj0->elem, &my_obj1->elem);
    }
    else //id == 1
    {
	p64_linklist_t *elem;
	while ((elem = ll_lookup(&ll_list, ll_elems[0].data)) == NULL)
	{
	    VERIFY_YIELD();
	}
	ll_remove(&ll_list, &ll_elems[0].elem);
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
	ll_insert(&ll_list, &ll_list, &my_obj0->elem);
	ll_insert(&ll_list, &my_obj0->elem, &my_obj1->elem);
    }
    else //id == 0
    {
	p64_linklist_t *elem;
	while ((elem = ll_lookup(&ll_list, ll_elems[0].data)) == NULL)
	{
	    VERIFY_YIELD();
	}
	ll_remove(&ll_list, &ll_elems[0].elem);
    }
}

struct ver_funcs ver_linklist3 =
{
    "linklist3", ver_linklist3_init, ver_linklist3_exec, ver_linklist3_fini
};

static void
ver_linklist4_init(uint32_t numthreads)
{
    if (numthreads != NUMTHREADS)
    {
	abort();
    }
    p64_linklist_init(&ll_list);
    ll_elems[0].data = 0;
    ll_elems[1].data = 1;
    ll_elems[2].data = 2;
    ll_elems[3].data = 3;
    p64_linklist_insert(&ll_list, &ll_elems[0].elem);
    p64_linklist_insert(&ll_elems[0].elem, &ll_elems[1].elem);
    p64_linklist_insert(&ll_elems[1].elem, &ll_elems[3].elem);
    uint32_t nelems = 0;
    struct object *obj = (struct object *)ll_list.next;
    while (obj != NULL && nelems < 256)
    {
	obj = (struct object *)obj->elem.next;
	nelems++;
    }
    VERIFY_ASSERT(nelems == 3);
}

static void
ver_linklist4_fini(uint32_t numthreads)
{
    (void)numthreads;
    uint32_t nelems = 0;
    struct object *obj = (struct object *)ll_list.next;
    while (obj != NULL && nelems < 256)
    {
	obj = (struct object *)obj->elem.next;
	nelems++;
    }
    VERIFY_ASSERT(nelems == 3);
}

static void
ver_linklist4_exec(uint32_t id)
{
    if (id == 0)
    {
	//Insert elem2 after elem1 (and before elem3)
	ll_insert(&ll_list, &ll_elems[1].elem, &ll_elems[2].elem);
	//if elem1 present: elem0, elem1, elem2, elem3
	//if elem1 removed: elem0, elem3, elem2
    }
    else //id == 1
    {
	//Remove elem1
	ll_remove(&ll_list, &ll_elems[1].elem);
    }
}

struct ver_funcs ver_linklist4 =
{
    "linklist4", ver_linklist4_init, ver_linklist4_exec, ver_linklist4_fini
};
