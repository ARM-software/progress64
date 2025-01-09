//Copyright (c) 2025, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "p64_cuckooht.h"
#include "p64_qsbr.h"
#include "os_abstraction.h"

#include "verify.h"

#define NUMTHREADS 2

#define container_of(pointer, type, member) \
    ((type *)(void *)(((char *)pointer) - offsetof(type, member)))

struct object
{
    _Alignas(64)
    p64_cuckooelem_t ce;
    uint32_t key;
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
ver_cuckooht1_init(uint32_t numthreads)
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
ver_cuckooht1_fini(uint32_t numthreads)
{
    (void)numthreads;
    p64_cuckooht_free(cht);
    p64_qsbr_unregister();
    p64_qsbr_free(cht_qsbr);
}

static void
ver_cuckooht1_exec(uint32_t id)
{
    bool success;
    (void)success;
    success = p64_cuckooht_insert(cht, &cht_elems[id].ce, compute_hash(cht_elems[id].key));
    VERIFY_ASSERT(success == true);
    success = p64_cuckooht_remove(cht, &cht_elems[id].ce, compute_hash(cht_elems[id].key));
    VERIFY_ASSERT(success == true);
}

struct ver_funcs ver_cuckooht1 =
{
    "cuckooht1", ver_cuckooht1_init, ver_cuckooht1_exec, ver_cuckooht1_fini
};
