//Copyright (c) 2025, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "p64_hopscotch.h"
#include "p64_qsbr.h"
#include "os_abstraction.h"

#include "verify.h"

#define NUMTHREADS 2

#define container_of(pointer, type, member) \
    ((type *)(void *)(((char *)pointer) - offsetof(type, member)))

struct object
{
    _Alignas(64)
    uint32_t key;
};

static p64_qsbrdomain_t *hht_qsbr;
static p64_hopscotch_t *hht;
static struct object hht_elems[NUMTHREADS];

static int
compare_hs_key(const void *elem, const void *key)
{
    const struct object *obj = elem;
    return obj->key - *(const uint32_t *)key;
}

static inline uint64_t
compute_hash(uint32_t key)
{
    (void)key;
    return 0;
}

static void
ver_hopscotch1_init(uint32_t numthreads)
{
    if (numthreads != NUMTHREADS)
    {
	abort();
    }
    hht_qsbr = p64_qsbr_alloc(10);
    VERIFY_ASSERT(hht_qsbr != NULL);
    p64_qsbr_register(hht_qsbr);
    hht = p64_hopscotch_alloc(24, 0, compare_hs_key, 0);
    VERIFY_ASSERT(hht != NULL);
    hht_elems[0].key = 242;
    hht_elems[1].key = 243;
}

static void
ver_hopscotch1_fini(uint32_t numthreads)
{
    (void)numthreads;
    p64_hopscotch_free(hht);
    p64_qsbr_unregister();
    p64_qsbr_free(hht_qsbr);
}

static void
ver_hopscotch1_exec(uint32_t id)
{
    bool success;
    (void)success;
    success = p64_hopscotch_insert(hht, &hht_elems[id], compute_hash(hht_elems[id].key));
    VERIFY_ASSERT(success == true);
    success = p64_hopscotch_remove(hht, &hht_elems[id], compute_hash(hht_elems[id].key));
    VERIFY_ASSERT(success == true);
}

struct ver_funcs ver_hopscotch1 =
{
    "hopscotch1", ver_hopscotch1_init, ver_hopscotch1_exec, ver_hopscotch1_fini
};
