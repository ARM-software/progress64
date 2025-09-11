//Copyright (c) 2025, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "p64_cuckooht.h"
#include "p64_qsbr.h"
#include "os_abstraction.h"

#include "atomic.h"
#include "verify.h"

#define NUMTHREADS 2

#define container_of(pointer, type, member) \
    ((type *)(void *)(((char *)pointer) - offsetof(type, member)))

struct object
{
    _Alignas(64)
    p64_cuckooelem_t ce;
    uint32_t key;
    uint32_t data;
};

static p64_qsbrdomain_t *cht_qsbr;
static p64_cuckooht_t *cht;
static struct object cht_elems[NUMTHREADS];

static int
compare_cc_key(const p64_cuckooelem_t *ce, const void *key)
{
    const struct object *obj = container_of(ce, struct object, ce);
    return obj->key - *(const uint32_t *)key;
}

static inline uint64_t
compute_hash(uint32_t key)
{
    (void)key;
    return 0;
}

static void
ver_cuckooht3_init(uint32_t numthreads)
{
    if (numthreads != NUMTHREADS)
    {
	abort();
    }
    cht_qsbr = p64_qsbr_alloc(10);
    VERIFY_ASSERT(cht_qsbr != NULL);
    p64_qsbr_register(cht_qsbr);
    cht = p64_cuckooht_alloc(16, 0, compare_cc_key, 0);
    VERIFY_ASSERT(cht != NULL);
    cht_elems[0].key = 242;
    cht_elems[1].key = 243;
}

static void
ver_cuckooht3_fini(uint32_t numthreads)
{
    bool success;
    (void)numthreads;
    success = p64_cuckooht_remove(cht, &cht_elems[0].ce, compute_hash(cht_elems[0].key));
    VERIFY_ASSERT(success == true);
    success = p64_cuckooht_remove(cht, &cht_elems[1].ce, compute_hash(cht_elems[1].key));
    VERIFY_ASSERT(success == true);
    p64_cuckooht_free(cht);
    p64_qsbr_unregister();
    p64_qsbr_free(cht_qsbr);
}

static void
ver_cuckooht3_exec(uint32_t id)
{
    bool success;
    (void)success;
    regular_store_n(&cht_elems[id].data, id);
    success = p64_cuckooht_insert(cht, &cht_elems[id].ce, compute_hash(cht_elems[id].key));
    VERIFY_ASSERT(success == true);
    //See if we can look up the element inserted by the other thread
    uint32_t other = 1 - id;
    p64_cuckooelem_t *elem = p64_cuckooht_lookup(cht, &cht_elems[other].key, compute_hash(cht_elems[other].key), NULL);
    if (elem != NULL)
    {
	//Read the data field to trigger the verifier to find a relevant synchronizes-with edge
	struct object *obj = (struct object *)elem;
	VERIFY_ASSERT(regular_load_n(&obj->data) == other);
    }
}

struct ver_funcs ver_cuckooht3 =
{
    "cuckooht3", ver_cuckooht3_init, ver_cuckooht3_exec, ver_cuckooht3_fini
};
