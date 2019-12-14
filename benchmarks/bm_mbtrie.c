//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#define _POSIX_C_SOURCE 199506L
#define _ISOC11_SOURCE
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "p64_mbtrie.h"
#include "p64_hashtable.h"
#include "p64_hopscotch.h"
#include "p64_qsbr.h"

void p64_hopscotch_check(p64_hopscotch_t *);

#define MAX_ROUTES 1000000
#define MAX_ASNODES 500000
#define HS_NUM_CELLS 100
#define NLOOKUPS 2000000
#define ALIGNMENT 64
#define BREAKUP(x) (x) >> 24, ((x) >> 16) & 0xff, ((x) >> 8) & 0xff, (x) & 0xff

#define LIKELY(x)    __builtin_expect(!!(x), 1)
#define UNLIKELY(x)  __builtin_expect(!!(x), 0)
#define MAX(a,b) ((a) > (b) ? (a) : (b))

#define ROUNDUP(a, b) \
    ({                                          \
        __typeof__ (a) tmp_a = (a);             \
        __typeof__ (b) tmp_b = (b);             \
        ((tmp_a + tmp_b - 1) / tmp_b) * tmp_b;  \
    })

#define container_of(pointer, type, member) \
    ((type *)(void *)(((char *)pointer) - offsetof(type, member)))

static void
verification_failed(const char *file, unsigned line, const char *exp)
{
    fprintf(stderr, "Verification failed at %s:%u '%s'\n", file, line, exp);
    fflush(NULL);
    abort();
}
#define verify(exp) \
{ \
    if (!(exp)) verification_failed(__FILE__, __LINE__, #exp); \
}

static bool verbose = false;
static bool use_hp = false;
static bool use_hs = false;
static bool use_vl = false;
static bool option_f = false;
static uint32_t num_rotations;
static uint32_t rebalance_ops;
static uint32_t nodes_moved;
static uint32_t histo[256];

static float
avg_depth(void)
{
    uint32_t histo_size = sizeof histo / sizeof histo[0];
    uint32_t n = histo[0];
    uint32_t sum = histo[0] * histo_size;
    for (uint32_t i = 1; i < histo_size; i++)
    {
	n += histo[i];
	sum += histo[i] * i;
    }
    return n != 0 ? (float)sum / n : 0;
}

struct asnode;

struct avl_route
{
    //Used by AVL tree
    struct avl_route *before;
    struct avl_route *after;
    struct avl_route *within;
    //pfx and mask used by find_lpm()
    uint32_t pfx;
    uint32_t mask;
    uint32_t random;
    uint16_t depth;
    uint8_t pfxlen;
    //Next hop information
    struct asnode *nexthop;
};

static void
update_histo(const struct avl_route *this, uint32_t depth)
{
    if (this == NULL)
    {
	return;
    }

    if (this->before != NULL)
    {
	update_histo(this->before, depth + 1);
    }
    if (this->after != NULL)
    {
	update_histo(this->after, depth + 1);
    }
    if (this->within != NULL)
    {
	update_histo(this->within, depth + 1);
    }
    if (this->before == NULL && this->after == NULL && this->within == NULL)
    {
	//Found leaf node
	if (depth < sizeof histo / sizeof histo[0])
	{
	    histo[depth]++;
	}
	else
	{
	    histo[0]++;//Overflow counter
	}
    }
}

static inline uint32_t
STARTS(const struct avl_route *rt)
{
    return rt->pfx;
}

static inline uint32_t
ENDS(const struct avl_route *rt)
{
    return rt->pfx + ~rt->mask;
}

static bool
insert_prefix(struct avl_route **parent_ptr, struct avl_route *newn);

static inline uint32_t
mask_from_len(uint32_t len)
{
    return len != 0U ? ~((UINT32_C(1) << (32 - len)) - 1) : 0;
}

static inline uint32_t
depth(const struct avl_route *node, uint32_t pfxlen)
{
    if (UNLIKELY(node == NULL))
    {
	return 0;
    }
    if (!option_f)
    {
	if (node->pfxlen != pfxlen)
	{
	    return 0;
	}
    }
    return node->depth;
}

static inline uint32_t
max_depth(const struct avl_route *node)
{
    uint32_t d_b = depth(node->before, node->pfxlen);
    uint32_t d_a = depth(node->after, node->pfxlen);
    return MAX(d_b, d_a);
}

enum l_type
{
    l_before, l_within, l_after
};

static void
verify_limits(const struct avl_route *this,
	      enum l_type type,
	      const struct avl_route *root)
{
    verify(this->pfxlen >= root->pfxlen);
    switch (type)
    {
	case l_before :
	    verify(ENDS(this) < STARTS(root));
	    break;
	case l_within :
	    verify(STARTS(this) >= STARTS(root) && ENDS(this) <= ENDS(root));
	    break;
	case l_after :
	    verify(STARTS(this) > ENDS(root));
	    break;
    }
    if (this->before != NULL)
    {
	verify_limits(this->before, type, root);
    }
    if (this->after != NULL)
    {
	verify_limits(this->after, type, root);
    }
    if (this->within != NULL)
    {
	verify_limits(this->within, type, root);
    }
}

static void
verify_prefix(const struct avl_route *this)
{
    if (this == NULL)
    {
	return;
    }

    verify(mask_from_len(this->pfxlen) == this->mask);

    verify(STARTS(this) <= ENDS(this));
    //Verify that all children conform to our start/end limits
    if (this->before != NULL)
    {
	verify(this->before->pfxlen >= this->pfxlen);
	verify_limits(this->before, l_before, this);
	verify_prefix(this->before);
    }
    if (this->after != NULL)
    {
	verify(this->after->pfxlen >= this->pfxlen);
	verify_limits(this->after, l_after, this);
	verify_prefix(this->after);
    }
    if (this->within != NULL)
    {
	verify(this->within->pfxlen > this->pfxlen);
	verify_limits(this->within, l_within, this);
	verify_prefix(this->within);
    }
}

#ifndef NDEBUG
#define assert_prefix(x) verify_prefix((x))
#else
#define assert_prefix(x) (void)(x)
#endif

static void
init_avl_route(struct avl_route *this,
	       uint32_t pfx,
	       uint32_t pfxlen,
	       unsigned rand,
	       struct asnode *nh)
{
    this->before = NULL;
    this->within = NULL;
    this->after = NULL;
    this->pfx = pfx;
    this->mask = mask_from_len(pfxlen);
    this->random = rand;
    this->pfxlen = pfxlen;
    this->depth = 1;
    this->nexthop = nh;
    assert_prefix(this);
}

static void
free_prefix(void *arg, p64_mbtrie_elem_t *ptr)
{
    (void)arg;
    (void)ptr;
    //Don't actually free element
}

//Return true if node may need to be rebalanced
static inline bool
recompute_depth(struct avl_route *node)
{
    uint32_t old_depth = node->depth;
    node->depth = 1 + max_depth(node);
    return old_depth != node->depth;
}

static inline bool
ok_to_rotate_right(const struct avl_route *A)
{
    if (A->before != NULL)
    {
	return A->pfxlen == A->before->pfxlen;
    }
    return false;
}

static inline bool
ok_to_rotate_left(const struct avl_route *A)
{
    if (A->after != NULL)
    {
	return A->pfxlen == A->after->pfxlen;
    }
    return false;
}

static struct avl_route *
rotate_right(struct avl_route *A) //A->before becomes new root
{
    assert(ok_to_rotate_right(A));
    struct avl_route *B = A->before;
    assert(B->pfxlen <= A->pfxlen);
    A->before = B->after;
    (void)recompute_depth(A);
    B->after = A;
    (void)recompute_depth(B);
    assert_prefix(B);
    num_rotations++;
    return B;
}

static struct avl_route *
rotate_left(struct avl_route *A) //A->after becomes new root
{
    assert(ok_to_rotate_left(A));
    struct avl_route *C = A->after;
    assert(C->pfxlen <= A->pfxlen);
    A->after = C->before;
    (void)recompute_depth(A);
    C->before = A;
    (void)recompute_depth(C);
    assert_prefix(C);
    num_rotations++;
    return C;
}

static void
rebalance(struct avl_route **parent_ptr)
{
    struct avl_route *A = *parent_ptr;
    int bal = depth(A->before, A->pfxlen) - depth(A->after, A->pfxlen);
    if (bal < -1) //DEPTH(before) << DEPTH(AFTER), new node inserted on right side
    {
	struct avl_route *C = A->after;
	if (depth(C->before, C->pfxlen) > depth(C->after, C->pfxlen))
	{
	    if (option_f && !ok_to_rotate_right(C)) return;
	    A->after = rotate_right(A->after);
	}
	if (option_f && !ok_to_rotate_left(A)) return;
	*parent_ptr = rotate_left(A);
	rebalance_ops++;
    }
    else
    if (bal > 1) //DEPTH(before) >> DEPTH(after), new node inserted on left side
    {
	struct avl_route *B = A->before;
	if (depth(B->after, B->pfxlen) > depth(B->before, B->pfxlen))
	{
	    if (option_f && !ok_to_rotate_left(B)) return;
	    A->before = rotate_left(A->before);
	}
	if (option_f && !ok_to_rotate_right(A)) return;
	*parent_ptr = rotate_right(A);
	rebalance_ops++;
    }
}

static void
insert_subtree(struct avl_route **root_ptr, struct avl_route *node)
{
    //Read pointers to outside children before the fields are cleared
    //Inside children are already in the right place
    assert_prefix(node);
    struct avl_route *before = node->before;
    struct avl_route *after = node->after;
    node->before = node->after = NULL;
    node->depth = max_depth(node);
    assert_prefix(node);
    nodes_moved++;
    insert_prefix(root_ptr, node);
    assert_prefix(node);
    assert_prefix(*root_ptr);
    //Now re-insert the outside children
    if (before != NULL)
    {
	assert_prefix(before);
	insert_subtree(root_ptr, before);
    }
    if (after != NULL)
    {
	assert_prefix(after);
	insert_subtree(root_ptr, after);
    }
}

static bool
insert_prefix(struct avl_route **parent_ptr, struct avl_route *newn)
{
    struct avl_route *curn = *parent_ptr;
    if (UNLIKELY(curn == NULL))
    {
	newn->before = NULL;
	newn->after = NULL;
	newn->depth = 1;
	//Leave inside pointer as is
	*parent_ptr = newn;
	assert_prefix(newn);
    }
    else if (LIKELY(newn->pfxlen >= curn->pfxlen))
    {
	//Insert new node below current node
	if (STARTS(newn) < STARTS(curn))
	{
	    assert(ENDS(newn) < STARTS(curn));
	    return insert_prefix(&curn->before, newn);
	}
	else if (STARTS(newn) > ENDS(curn))
	{
	    assert(STARTS(newn) > ENDS(curn));
	    return insert_prefix(&curn->after, newn);
	}
	else
	{
	    assert(STARTS(newn) >= STARTS(curn));
	    assert(ENDS(newn) <= ENDS(curn));
	    if (newn->pfxlen > curn->pfxlen)
	    {
		return insert_prefix(&curn->within, newn);
	    }
	    else
	    {
		assert(STARTS(newn) == STARTS(curn));
		assert(ENDS(newn) == ENDS(curn));
		//Duplicate route
		fprintf(stderr, "Ignoring duplicate route %u.%u.%u.%u/%u\n",
			BREAKUP(newn->pfx), newn->pfxlen);
	    }
	    return false;
	}
	if (recompute_depth(curn))
	{
	    rebalance(parent_ptr);
	}
    }
    else//if (newn->pfxlen < curn->pfxlen)
    {
	assert(newn->pfxlen < curn->pfxlen);
	//Insert new node above current node to preserve pfxlen ordering
	struct avl_route *to_moveA = NULL, *to_moveB = NULL;
	if (STARTS(curn) < STARTS(newn))
	{
	    assert(ENDS(curn) < STARTS(newn));
	    *parent_ptr = newn;
	    newn->before = curn;
	    newn->after = NULL;
	    //Routes after curn might have to be moved
	    to_moveA = curn->after;
	    curn->after = NULL;
	    if (recompute_depth(curn))
	    {
		rebalance(&newn->before);
	    }
	    if (recompute_depth(*parent_ptr))
	    {
		rebalance(parent_ptr);
	    }
	}
	else if (STARTS(curn) > ENDS(newn))
	{
	    *parent_ptr = newn;
	    newn->before = NULL;
	    newn->after = curn;
	    //Routes before curn might need to be moved
	    to_moveB = curn->before;
	    curn->before = NULL;
	    if (recompute_depth(curn))
	    {
		rebalance(&newn->after);
	    }
	    if (recompute_depth(*parent_ptr))
	    {
		rebalance(parent_ptr);
	    }
	}
	else//curn within newn, insert (replace) new node above current node
	{
	    *parent_ptr = newn;
	    newn->before = NULL;
	    newn->after = NULL;
	    newn->within = curn;
	    //Routes before curn might need to be moved
	    to_moveB = curn->before;
	    curn->before = NULL;
	    //Routes after curn might have to be moved
	    to_moveA = curn->after;
	    curn->after = NULL;
	}
	assert_prefix(newn);
        if (to_moveB != NULL)
	{
	    insert_subtree(parent_ptr, to_moveB);
	}
	if (to_moveA != NULL)
	{
	    insert_subtree(parent_ptr, to_moveA);
	}
	assert_prefix(newn);
    }
    if (!option_f)
    {
	struct avl_route *root = *parent_ptr;
	int balance = depth(root->before, root->pfxlen) -
		      depth(root->after, root->pfxlen);
	assert(balance >= -1 && balance <= 1);
	(void)balance;
    }
    return true;
}

__attribute_noinline__
static struct avl_route *
find_lpm(const struct avl_route *node, uint32_t key, struct avl_route *lpm)
{
    while (LIKELY(node != NULL))
    {
	struct avl_route *before = node->before;
	struct avl_route *after = node->after;
	if (LIKELY(key - node->pfx > ~node->mask))
	{
	    assert(key < STARTS(node) || key > ENDS(node));
	    assert(((key ^ node->pfx) & node->mask) != 0);
	    node = key < node->pfx ? before : after;
	}
	else
	{
	    assert(key >= STARTS(node) && key <= ENDS(node));
	    assert(((key ^ node->pfx) & node->mask) == 0);
	    lpm = (struct avl_route *)node;
	    node = node->within;
	}
    }
    return lpm;
}

static int
compare_random(const void *a, const void *b)
{
    const struct avl_route *pa = a;
    const struct avl_route *pb = b;
    return pa->random - pb->random;
}

static int
compare_increasing(const void *a, const void *b)
{
    const struct avl_route *pa = a;
    const struct avl_route *pb = b;
    if (pa->pfxlen != pb->pfxlen)
    {
	return pa->pfxlen - pb->pfxlen;
    }
    return pa->pfx - pb->pfx;
}

static void
mbt_count_cb(void *arg,
	     uint64_t pfx,
	     uint32_t pfxlen,
	     p64_mbtrie_elem_t *elem,
	     uint32_t actlen)
{
    (void)pfx;
    (void)pfxlen;
    (void)elem;
    (void)actlen;
    assert(elem != NULL);
    (*(size_t *)arg)++;
}

static size_t
mbt_count_elems(p64_mbtrie_t *mbt)
{
    size_t nelems = 0;
    p64_mbtrie_traverse(mbt, mbt_count_cb, &nelems, false);
    return nelems;
}

static uint64_t
time_start(const char *msg)
{
    struct timespec ts;
    printf("%s: ", msg); fflush(stdout);
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void
time_stop(uint64_t start, uint32_t n)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t elapsed = ts.tv_sec * 1000000000ULL + ts.tv_nsec - start;
    printf("%"PRIu64".%09"PRIu64" seconds (%u items)\n", elapsed / 1000000000 , elapsed % 1000000000, n);
    if (n != 0)
    {
	elapsed /= n;
	printf("%"PRIu64" nanoseconds/item\n", elapsed);
    }
}

struct asnode
{
    //Used by multibit trie
    p64_mbtrie_elem_t mbe;
    //Used by hash table
    p64_hashelem_t he;
    p64_hashvalue_t hash;//Hash of the key
    uint32_t asn;//The key
    uint32_t gw;//IP address of gateway
    uint16_t ifx;//Interface index
    uint8_t macaddr[6];//MAC address
    char name[];
};

//Compare two keys for less/equal/greater, returning -1/0/1
static int
compf(const p64_hashelem_t *he,
      const void *key)
{
    const struct asnode *as = container_of(he, struct asnode, he);
    uint32_t k = *(const uint32_t*)key;
    return as->asn < k ? -1 : as->asn > k ? 1 : 0;
}

//Compare two keys for less/equal/greater, returning -1/0/1
static int
comph(const void *he,
      const void *key)
{
    const struct asnode *as = he;
    uint32_t k = *(const uint32_t*)key;
    return as->asn < k ? -1 : as->asn > k ? 1 : 0;
}

//Some magic to define CRC32C intrinsics
#ifdef __aarch64__
#ifndef __ARM_FEATURE_CRC32
#pragma GCC target("+crc")
#endif
#include <arm_acle.h>
#define CRC32C(x, y) __crc32cw((x), (y))
#endif
#if defined __x86_64__
#ifndef __SSE4_2__
#pragma GCC target("sse4.2")
#endif
#include <x86intrin.h>
//x86 crc32 intrinsics seem to compute CRC32C (not CRC32)
#define CRC32C(x, y) __crc32d((x), (y))
#endif

static uint32_t
read_as_table(const char *filename,
	      void *ht)
{
    printf("Read AS data from file \"%s\"\n", filename);
    uint64_t start = time_start("Read AS data, insert into hash table");
    FILE *fp = fopen(filename, "r");
    if (fp == NULL)
    {
	fprintf(stderr, "Failed to open file %s, error %d\n", filename, errno);
	exit(EXIT_FAILURE);
    }
    uint32_t nasnodes = 0;
    unsigned asn;
    while (fscanf(fp, " %u\n", &asn) == 1)
    {
	int c;
	while ((c = fgetc(fp)) == ' ')
	{
	}
	char name[80];
	unsigned l = 0;
	while (c != '\n' && c != EOF)
	{
	    if (l < sizeof name - 1)
	    {
		name[l++] = c;
	    }
	    c = fgetc(fp);
	}
	assert(l < sizeof name);
	name[l++] = '\0';
	if (verbose)
	{
	    printf("%u %s\n", asn, name);
	}
	size_t sz = sizeof(struct asnode) + l;
	struct asnode *as = aligned_alloc(ALIGNMENT, ROUNDUP(sz, ALIGNMENT));
	if (as == NULL)
	{
	    perror("malloc"), exit(EXIT_FAILURE);
	}
	as->asn = asn;
	as->hash = CRC32C(0, asn);
	strcpy(as->name, name);
	if (use_hs)
	{
	    if (!p64_hopscotch_insert(ht, as, as->hash))
	    {
		fprintf(stderr, "Failed to insert ASN %u\n", as->asn);
		free(as);
		nasnodes--;//Neutralize increment below
	    }
	}
	else
	{
	    p64_hashtable_insert(ht, &as->he, as->hash);
	}
	nasnodes++;
    }
    fclose(fp);
    time_stop(start, nasnodes);
    printf("Read %u AS entries\n", nasnodes);
    return nasnodes;
}

static struct avl_route *
read_rt_table(const char *filename,
	      uint32_t nelems,
	      void *ht,
	      uint32_t *nroutes)
{
    printf("Read routes from file \"%s\"\n", filename);
    uint64_t start = time_start("Read routes from file");
    struct avl_route *routes = malloc(nelems * sizeof(struct avl_route));
    if (routes == NULL)
    {
	perror("malloc"), exit(EXIT_FAILURE);
    }
    FILE *file = fopen(filename, "r");
    if (file == NULL)
    {
	fprintf(stderr, "Failed to open file %s, error %d\n", filename, errno);
	exit(EXIT_FAILURE);
    }
    uint32_t n = 0, skipped = 0;
    uint32_t prev_dest = 0, prev_l = 0;
    unsigned seed = 242;
    p64_hazardptr_t hp = P64_HAZARDPTR_NULL;
    while (!feof(file))
    {
	uint32_t a, b, c, d, l, pfx, asn;
	if (fscanf(file, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
	{
syntax_error:
	    fprintf(stderr, "Syntax error on line %u\n", n + 1);
	    exit(EXIT_FAILURE);
	}
	pfx = a << 24 | b << 16 | c << 8 | d;
	if (fscanf(file, "/%u", &l) == 0)
	{
	    fprintf(stderr, "Invalid prefix %u.%u.%u.%u\n", a, b, c, d);
	    exit(EXIT_FAILURE);
	}
	if (fscanf(file, "%u\n", &asn) != 1)
	{
	    goto syntax_error;
	}
	if ((pfx & ~mask_from_len(l)) != 0)
	{
	    fprintf(stderr, "Prefix %u.%u.%u.%u/%u has unused bits set\n",
		    a, b, c, d, l);
	    exit(EXIT_FAILURE);
	}
	//Assume all routes refer to remote networks
	if (pfx != prev_dest || l != prev_l)
	{
	    if (n == nelems)
	    {
		fprintf(stderr, "Too many routes\n");
		exit(EXIT_FAILURE);
	    }
	    p64_hashvalue_t hash = CRC32C(0, asn);
	    struct asnode *as = NULL;
	    if (use_hs)
	    {
		if (use_vl)
		{
		    const void *keys[1] = { &asn };
		    p64_hopschash_t hashes[1] = { hash };
		    void *res[1];
		    unsigned long m;
		    m = p64_hopscotch_lookup_vec(ht, 1, keys, hashes, res);
		    (void)m;
		    as = res[0];
		    assert((m == 0 && as == NULL) || (m == 1 && as != NULL));
		}
		else
		{
		    as = p64_hopscotch_lookup(ht, &asn, hash, &hp);
		}
	    }
	    else
	    {
		p64_hashelem_t *he = p64_hashtable_lookup(ht, &asn, hash, &hp);
		if (he != NULL)
		{
		    as = container_of(he, struct asnode, he);
		}
	    }
	    if (as == NULL)
	    {
		fprintf(stderr, "Failed to lookup ASN %u\n", asn);
		skipped++;
	    }
	    else
	    {
		assert(as->asn == asn);
		init_avl_route(&routes[n], pfx, l, rand_r(&seed), as);
		n++;
		prev_dest = pfx;
		prev_l = l;
	    }
	}
	else
	{
	    skipped++;
	}
    }
    if (use_hp)
    {
	p64_hazptr_release(&hp);
    }
    fclose(file);
    time_stop(start, n + skipped);
    printf("Read %u routes (skipped %u)\n", n + skipped, skipped);
    *nroutes = n;
    return routes;
}

static void
hs_free_cb(void *arg,
	   void *elem,
	   size_t idx)
{
    (void)idx;
    p64_hopscotch_t *ht = arg;
    struct asnode *as = elem;
    if (!p64_hopscotch_remove(ht, as, as->hash))
    {
	fprintf(stderr,
		"Failed to remove element (ASN %u, hash %"PRIu64") "
		"from hash table\n",
		as->asn, as->hash);
	return;
    }
    bool retire_ok;
    if (use_hp)
    {
	retire_ok = p64_hazptr_retire(elem, free);
    }
    else
    {
	retire_ok = p64_qsbr_retire(elem, free);
    }
    if (!retire_ok)
    {
	fprintf(stderr, "Failed to retire element\n");
	exit(EXIT_FAILURE);
    }
}

static void
free_as(void *ptr)
{
    //Convert hashelem pointer to asnode pointer that was malloc'ed
    struct asnode *as = container_of(ptr, struct asnode, he);
    free(as);
}

static void
ht_free_cb(void *arg,
	   p64_hashelem_t *he,
	   size_t idx)
{
    (void)idx;
    p64_hashtable_t *ht = arg;
    struct asnode *as = container_of(he, struct asnode, he);
    if (!p64_hashtable_remove(ht, he, as->hash))
    {
	fprintf(stderr, "Failed to remove element (ASN %u) from hash table\n",
		as->asn);
	return;
    }
    //We must retire 'he', the pointer as seen by the hash table
    bool retire_ok;
    if (use_hp)
    {
	retire_ok = p64_hazptr_retire(he, free_as);
    }
    else
    {
	retire_ok = p64_qsbr_retire(he, free_as);
    }
    if (!retire_ok)
    {
	fprintf(stderr, "Failed to retire element\n");
	exit(EXIT_FAILURE);
    }
}

static void
hs_count_cb(void *arg,
	    void *he,
	    size_t idx)
{
    (void)he;
    (void)idx;
    assert(he != NULL);
    (*(size_t *)arg)++;
}

static inline size_t
hs_count_elems(void *ht)
{
    size_t nelems = 0;
    p64_hopscotch_traverse(ht, hs_count_cb, &nelems);
    return nelems;
}

static void
ht_count_cb(void *arg,
	    p64_hashelem_t *he,
	    size_t idx)
{
    (void)he;
    (void)idx;
    assert(he != NULL);
    (*(size_t *)arg)++;
}

static inline size_t
ht_count_elems(p64_hashtable_t *ht)
{
    size_t nelems = 0;
    p64_hashtable_traverse(ht, ht_count_cb, &nelems);
    return nelems;
}

int main(int argc, char *argv[])
{
    bool do_avl = false;
    bool do_random = false;
    uint32_t hp_refs = 2;
    uint32_t maxroutes = MAX_ROUTES;
    uint32_t numbkts = MAX_ASNODES;
    uint32_t numcells = HS_NUM_CELLS;
    uint32_t vecsize = 16;
    uint64_t start;
    struct avl_route *root = NULL;
    p64_mbtrie_t *mbt = NULL;

    struct stat sb;
    if (stat("data-raw-table", &sb) != 0 ||
	stat("data-used-autnums", &sb) != 0)
    {
	fprintf(stderr, "Download BGP data from e.g. http://thyme.apnic.net/current/data-raw-table and http://thyme.apnic.net/current/data-used-autnums\n");
	exit(EXIT_FAILURE);
    }

    assert(mask_from_len( 0) == 0x00000000);
    assert(mask_from_len( 8) == 0xff000000);
    assert(mask_from_len(16) == 0xffff0000);
    assert(mask_from_len(24) == 0xffffff00);
    assert(mask_from_len(32) == 0xffffffff);

    int c;
    while ((c = getopt(argc, argv, "Ab:c:Fhm:r:sRv:V")) != -1)
    {
	switch (c)
	{
	    case 'A' :
		do_avl = true;
		break;
	    case 'b' :
		numbkts = atoi(optarg);
		break;
	    case 'c' :
		numcells = atoi(optarg);
		break;
	    case 'F' :
		option_f = true;
		break;
	    case 'h' :
		use_hp = true;
		break;
	    case 'm' :
		maxroutes = atoi(optarg);
		break;
	    case 'r' :
		hp_refs = atoi(optarg);
		break;
	    case 'R' :
		do_random = true;
		break;
	    case 's' :
		use_hs = true;
		break;
	    case 'v' :
	    {
		int32_t vz = atoi(optarg);
		if (vz < 0 || (uint32_t)vz > sizeof(long) * CHAR_BIT)
		{
		    fprintf(stderr, "Invalid vector size %s\n", optarg);
		    exit(EXIT_FAILURE);
		}
		vecsize = vz;
		use_vl = true;
		break;
	    }
	    case 'V' :
		verbose = true;
		break;
	    default :
usage :
	    fprintf(stderr,
		    "Usage: route <options>\n"
		    "-A               Use AVL tree\n"
		    "-b <numbkts>     Number of hash table buckets\n"
		    "-c <numcells>    Size of hopscotch cellar\n"
		    "-F               Flatten AVL tree\n"
		    "-h               Use hazard pointers\n"
		    "-m <maxprefixes> Maximum number of prefixes\n"
		    "-r <maxrefs>     Number of HP references\n"
		    "-R               Randomize AVL tree insertion order\n"
		    "-s               Use hopscotch hash table\n"
		    "-v <vecsize>     Use vector lookup\n"
		    "-V               Verbose\n");
	    exit(EXIT_FAILURE);
	}
    }
    if (optind > argc)
    {
	goto usage;
    }

    p64_hpdomain_t *hpd = NULL;
    p64_qsbrdomain_t *qsbrd = NULL;
    if (use_hp)
    {
	printf("Using hazard pointers (nrefs=%u) for safe memory reclamation\n",
		hp_refs);
	hpd = p64_hazptr_alloc(100, hp_refs);
	p64_hazptr_register(hpd);
	assert(p64_hazptr_dump(stdout) == hp_refs);
    }
    else
    {
	printf("Using QSBR for safe memory reclamation\n");
	qsbrd = p64_qsbr_alloc(1000);
	p64_qsbr_register(qsbrd);
    }

    void *ht;
    if (use_hs)
    {
	ht = p64_hopscotch_alloc(numbkts, numcells, comph, use_hp ? P64_HOPSCOTCH_F_HP : 0);
    }
    else
    {
	ht = p64_hashtable_alloc(numbkts, compf, use_hp ? P64_HASHTAB_F_HP : 0);
    }
    if (ht == NULL)
    {
	perror("malloc"), exit(EXIT_FAILURE);
    }
    uint32_t nasnodes = read_as_table("data-used-autnums", ht);
    (void)nasnodes;
    if (use_hs)
    {
	p64_hopscotch_check(ht);
	assert(hs_count_elems(ht) == nasnodes);
    }
    else
    {
	assert(ht_count_elems(ht) == nasnodes);
    }

    struct avl_route *routes;
    uint32_t nroutes;
    routes = read_rt_table("data-raw-table", maxroutes, ht, &nroutes);
    if (nroutes == 0)
    {
	fprintf(stderr, "No routes found\n");
	exit(EXIT_FAILURE);
    }

    //Sort routes on random tag or based on pfxlen+address
    qsort(routes,
	  nroutes,
	  sizeof(struct avl_route),
	  do_random ? compare_random : compare_increasing);

    if (do_avl)
    {
	uint32_t nfailed = 0;
	nodes_moved = 0;
	rebalance_ops = 0;
	num_rotations = 0;
	start = time_start("Insert routes into AVL tree");
	for (uint32_t i = 0; i < nroutes; i++)
	{
	    if (!insert_prefix(&root, &routes[i]))
	    {
		nfailed++;
	    }
	}
	time_stop(start, nroutes);
	printf("Inserted %u routes (%u failed) in %s order\n",
		nroutes, nfailed,
		do_random ? "random" : "increasing");
	printf("%u rebalance ops, %u rotations, %u nodes moved\n",
		rebalance_ops, num_rotations, nodes_moved);
	printf("Verify AVL tree\n");
	verify_prefix(root);
	update_histo(root, 1);
	printf("Average depth %.1f\n", avg_depth());
    }
    else
    {
	mbt = p64_mbtrie_alloc((uint8_t []){ 24, 8, 0 },
			       free_prefix,
			       NULL,
			       use_hp ? P64_MBTRIE_F_HP : 0);
	if (mbt == NULL)
	{
	    perror("malloc"), exit(EXIT_FAILURE);
	}
	start = time_start("Insert prefixes into multi-bit trie");
	for (uint32_t i = 0; i < nroutes; i++)
	{
	    struct avl_route *rt = &routes[i];
	    uint64_t pfx = (uint64_t)rt->pfx << 32;
	    p64_mbtrie_insert(mbt, pfx, rt->pfxlen, &rt->nexthop->mbe);
	}
	time_stop(start, nroutes);
	printf("%zu prefixes found in multi-bit trie\n", mbt_count_elems(mbt));
    }

    unsigned seed = 242;
    uint32_t *addrs = malloc(NLOOKUPS * sizeof(uint32_t));
    if (addrs == NULL)
    {
	perror("malloc"), exit(EXIT_FAILURE);
    }
    for (uint32_t i = 0; i < NLOOKUPS; i++)
    {
	struct avl_route *rt = &routes[rand_r(&seed) % nroutes];
	//Random address might actually match another route
	addrs[i] = rt->pfx + (rand_r(&seed) & ~rt->mask);
    }

    if (do_avl)
    {
	uint32_t found = 0;
	start = time_start("Lookup routes in AVL tree");
	for (uint32_t i = 0; i < NLOOKUPS; i++)
	{
	    struct avl_route *rt = find_lpm(root, addrs[i], NULL);
	    if (rt != NULL)
		found++;
	}
	time_stop(start, NLOOKUPS);
	printf("%u hits (%.1f%%)\n", found, 100 * (float)found / NLOOKUPS);
    }
    else
    {
	uint32_t found = 0;
	if (use_hp)
	{
	    start = time_start("Lookup prefixes (scalar+HP) in multi-bit trie");
	    assert(p64_hazptr_dump(stdout) == hp_refs);
	    //Use single-key lookup which uses hazard pointers
	    p64_hazardptr_t hp = P64_HAZARDPTR_NULL;
	    for (uint32_t i = 0; i < NLOOKUPS; i++)
	    {
		uint64_t key = (uint64_t)addrs[i] << 32;
		p64_mbtrie_elem_t *elem = p64_mbtrie_lookup(mbt, key, &hp);
		if (elem != NULL)
		{
		    found++;
		    //Force a memory access from returned element
		    *(volatile size_t *)&elem->refcnt;
		}
	    }
	    p64_hazptr_release(&hp);
	    assert(p64_hazptr_dump(stdout) == hp_refs);
	}
	else if (vecsize == 0)
	{
	    start = time_start("Lookup prefixes (scalar+QSBR) in multi-bit trie");
	    p64_qsbr_acquire();
	    p64_hazardptr_t hp = P64_HAZARDPTR_NULL;
	    for (uint32_t i = 0; i < NLOOKUPS; i++)
	    {
		uint64_t key = (uint64_t)addrs[i] << 32;
		p64_mbtrie_elem_t *elem = p64_mbtrie_lookup(mbt, key, &hp);
		if (elem != NULL)
		{
		    found++;
		    //Force a memory access from returned element
		    *(volatile size_t *)&elem->refcnt;
		}
	    }
	    assert(hp == P64_HAZARDPTR_NULL);
	    p64_qsbr_release();
	}
	else
	{
	    char msg[100];
	    sprintf(msg, "Lookup prefixes (vector(%u)+QSBR) in multi-bit trie",
		    vecsize);
	    start = time_start(msg);
	    //Use multi-key lookup which requires application to use QSBR
	    p64_qsbr_acquire();
	    for (uint32_t i = 0; i + vecsize < NLOOKUPS; )
	    {
		p64_mbtrie_elem_t *results[vecsize];
		uint64_t keys[vecsize];
		//Keys start from most significant bit in uint64_t
		for (uint32_t j = 0; j < vecsize; j++)
		{
		    keys[j] = (uint64_t)addrs[i++] << 32;
		}
		(void)p64_mbtrie_lookup_vec(mbt, vecsize, keys, results);
		for (uint32_t j = 0; j < vecsize; j++)
		{
		    if (LIKELY(results[j] != NULL))
		    {
			//Force a memory access from returned element
			*(volatile size_t *)&results[j]->refcnt;
		    }
		}
	    }
	    p64_qsbr_release();//No references kept beyond this point
	}
	time_stop(start, NLOOKUPS);
	printf("%u hits (%.1f%%)\n", found, 100 * (float)found / NLOOKUPS);
    }

    if (!do_avl)
    {
	start = time_start("Remove prefixes from multi-bit trie");
	for (uint32_t i = 0; i < nroutes; i++)
	{
	    struct avl_route *rt = &routes[i];
	    uint64_t pfx = (uint64_t)rt->pfx << 32;
	    p64_mbtrie_remove(mbt, pfx, rt->pfxlen, &rt->nexthop->mbe, NULL);
	}
	time_stop(start, nroutes);
	assert(mbt_count_elems(mbt) == 0);
	p64_mbtrie_free(mbt);
    }

    start = time_start("Remove AS nodes from hash table");
    if (use_hs)
    {
	p64_hopscotch_traverse(ht, hs_free_cb, ht);
    }
    else
    {
	p64_hashtable_traverse(ht, ht_free_cb, ht);
    }
    time_stop(start, nroutes);
    if (use_hs)
    {
	assert(hs_count_elems(ht) == 0);
	p64_hopscotch_free(ht);
    }
    else
    {
	assert(ht_count_elems(ht) == 0);
	p64_hashtable_free(ht);
    }

    if (use_hp)
    {
	p64_hazptr_dump(stdout);
	p64_hazptr_reclaim();//Reclaim (free) any pending removed objects
	p64_hazptr_unregister();
	p64_hazptr_free(hpd);
    }
    else
    {
	p64_qsbr_reclaim();//Reclaim (free) any pending removed objects
	p64_qsbr_unregister();
	p64_qsbr_free(qsbrd);
    }
    free(addrs);
    free(routes);
    return 0;
}
