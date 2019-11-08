//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#define _POSIX_C_SOURCE 199506L
#define _ISOC11_SOURCE
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "p64_mbtrie.h"
#include "p64_qsbr.h"

#if defined __GNUC__
#define LIKELY(x)    __builtin_expect(!!(x), 1)
#define UNLIKELY(x)  __builtin_expect(!!(x), 0)
#else
#define LIKELY(x)    (x)
#define UNLIKELY(x)  (x)
#endif

#define BREAKUP(x) (x) >> 24, ((x) >> 16) & 0xff, ((x) >> 8) & 0xff, (x) & 0xff

struct prefix
{
    //Used by multibit trie
    p64_mbtrie_elem_t mbe;
    //Used by AVL tree
    struct prefix *before;
    struct prefix *after;
    struct prefix *within;
    //pfx and mask used by find_lpm()
    uint32_t pfx;
    uint32_t mask;
    uint32_t random;
    uint16_t depth;
    uint8_t pfxlen;
    //Next hop information
    union
    {
	void *ptr;
	uintptr_t uip;
    } nexthop;
};

#define MAX(a,b) ((a) > (b) ? (a) : (b))

#define STARTS(n) ((n)->pfx)
#define ENDS(n) ((n)->pfx + ~(n)->mask)

static bool option_f;

static void insert_prefix(struct prefix **parent_ptr, struct prefix *newn);

#ifndef NDEBUG
static inline uint32_t
len_from_mask(uint32_t mask)
{
    return mask != 0U ? 32U - __builtin_ctz(mask) : 0U;
}
#endif

static inline uint32_t
mask_from_len(uint32_t len)
{
    return len != 0U ? ~((UINT32_C(1) << (32 - len)) - 1) : 0;
}

static inline uint32_t
depth(const struct prefix *node, uint32_t pfxlen)
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
max_depth(const struct prefix *node)
{
    uint32_t d_b = depth(node->before, node->pfxlen);
    uint32_t d_a = depth(node->after, node->pfxlen);
    return MAX(d_b, d_a);
}

enum l_type
{
    l_before, l_within, l_after
};

#ifndef NDEBUG
static void
assert_limits(const struct prefix *this, enum l_type type, const struct prefix *root)
{
    assert(this->pfxlen >= root->pfxlen);
    switch (type)
    {
        case l_before :
            assert(ENDS(this) < STARTS(root));
            break;
        case l_within :
            assert(STARTS(this) >= STARTS(root) && ENDS(this) <= ENDS(root));
            break;
        case l_after :
            assert(STARTS(this) > ENDS(root));
            break;
    }
    if (this->before != NULL)
        assert_limits(this->before, type, root);
    if (this->after != NULL)
        assert_limits(this->after, type, root);
    if (this->within != NULL)
        assert_limits(this->within, type, root);
}

static void
assert_prefix(const struct prefix *this)
{
    if (this == NULL)
    {
        return;
    }

    assert(len_from_mask(this->mask) == this->pfxlen);
    assert(mask_from_len(this->pfxlen) == this->mask);

    assert(STARTS(this) <= ENDS(this));
    //Verify that all children conform to our start/end limits
    if (this->before != NULL)
    {
        assert(this->before->pfxlen >= this->pfxlen);
        assert_limits(this->before, l_before, this);
        assert_prefix(this->before);
    }
    if (this->after != NULL)
    {
        assert(this->after->pfxlen >= this->pfxlen);
        assert_limits(this->after, l_after, this);
        assert_prefix(this->after);
    }
    if (this->within != NULL)
    {
        assert(this->within->pfxlen > this->pfxlen);
        assert_limits(this->within, l_within, this);
        assert_prefix(this->within);
    }
}
#else
#define assert_prefix(x) (void)(x)
#endif

#define ROUNDUP(a, b) \
    ({                                          \
        __typeof__ (a) tmp_a = (a);             \
        __typeof__ (b) tmp_b = (b);             \
        ((tmp_a + tmp_b - 1) / tmp_b) * tmp_b;  \
    })

static struct prefix *
alloc_prefix(uint32_t d, uint32_t l, uint32_t gw, unsigned rand)
{
    struct prefix *this;
    this = aligned_alloc(64, ROUNDUP(sizeof(struct prefix), 64));
    if (this == NULL)
    {
        perror("malloc"), abort();
    }
    this->mbe.refcnt = 0;
    this->before = NULL;
    this->within = NULL;
    this->after = NULL;
    this->pfx = d;
    this->mask = mask_from_len(l);
    this->random = rand;
    this->pfxlen = l;
    this->depth = 1;
    this->nexthop.uip = gw;
    assert_prefix(this);
    return this;
}

static void
route_free(void *arg, p64_mbtrie_elem_t *ptr)
{
    (void)arg;
    (void)ptr;
    //Don't actually free element
}

//Return true if node may need to be rebalanced
static inline bool
recompute_depth(struct prefix *node)
{
    uint32_t old_depth = node->depth;
    node->depth = 1 + max_depth(node);
    return old_depth != node->depth;
}

static inline bool
ok_to_rotate_right(const struct prefix *A)
{
    if (A->before != NULL)
    {
	return A->pfxlen == A->before->pfxlen;
    }
    return false;
}

static inline bool
ok_to_rotate_left(const struct prefix *A)
{
    if (A->after != NULL)
    {
	return A->pfxlen == A->after->pfxlen;
    }
    return false;
}

static struct prefix *
rotate_right(struct prefix *A) //A->before becomes new root
{
    assert(ok_to_rotate_right(A));
    struct prefix *B = A->before;
    assert(B->pfxlen <= A->pfxlen);
    A->before = B->after;
    (void)recompute_depth(A);
    B->after = A;
    (void)recompute_depth(B);
    assert_prefix(B);
    return B;
}

static struct prefix *
rotate_left(struct prefix *A) //A->after becomes new root
{
    assert(ok_to_rotate_left(A));
    struct prefix *C = A->after;
    assert(C->pfxlen <= A->pfxlen);
    A->after = C->before;
    (void)recompute_depth(A);
    C->before = A;
    (void)recompute_depth(C);
    assert_prefix(C);
    return C;
}

static uint32_t nrebalance;

static void
rebalance(struct prefix **parent_ptr)
{
    struct prefix *A = *parent_ptr;
    int bal = depth(A->before, A->pfxlen) - depth(A->after, A->pfxlen);
    if (bal < -1) //DEPTH(before) << DEPTH(AFTER), new node inserted on right side
    {
	struct prefix *C = A->after;
	if (depth(C->before, C->pfxlen) > depth(C->after, C->pfxlen))
	{
	    if (option_f && !ok_to_rotate_right(C)) return;
	    A->after = rotate_right(A->after);
	}
	if (option_f && !ok_to_rotate_left(A)) return;
	*parent_ptr = rotate_left(A);
	nrebalance++;
    }
    else if (bal > 1) //DEPTH(before) >> DEPTH(after), new node inserted on left side
    {
	struct prefix *B = A->before;
	if (depth(B->after, B->pfxlen) > depth(B->before, B->pfxlen))
	{
	    if (option_f && !ok_to_rotate_left(B)) return;
	    A->before = rotate_left(A->before);
	}
	if (option_f && !ok_to_rotate_right(A)) return;
	*parent_ptr = rotate_right(A);
	nrebalance++;
    }
}

static uint32_t nmoved;

static void
insert_subtree(struct prefix **root_ptr, struct prefix *node)
{
    //Read pointers to outside children before the fields are cleared
    //Inside children are already in the right place
    assert_prefix(node);
    struct prefix *before = node->before;
    struct prefix *after = node->after;
    node->before = node->after = NULL;
    node->depth = max_depth(node);
    assert_prefix(node);
nmoved++;
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

static void
insert_prefix(struct prefix **parent_ptr, struct prefix *newn)
{
    struct prefix *curn = *parent_ptr;
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
	    insert_prefix(&curn->before, newn);
	}
	else if (STARTS(newn) > ENDS(curn))
	{
	    assert(STARTS(newn) > ENDS(curn));
	    insert_prefix(&curn->after, newn);
	}
	else
	{
	    assert(STARTS(newn) >= STARTS(curn));
	    assert(ENDS(newn) <= ENDS(curn));
	    if (newn->pfxlen > curn->pfxlen)
	    {
		insert_prefix(&curn->within, newn);
	    }
	    else
	    {
		assert(STARTS(newn) == STARTS(curn));
		assert(ENDS(newn) == ENDS(curn));
		//Duplicate route
		fprintf(stderr, "Ignoring duplicate route %u.%u.%u.%u/%u\n",
			BREAKUP(newn->pfx), newn->pfxlen);
	    }
	    return;
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
        struct prefix *to_move;
        if (STARTS(curn) < STARTS(newn))
        {
            assert(ENDS(curn) < STARTS(newn));
	    *parent_ptr = newn;
            newn->before = curn;
            newn->after = NULL;
            //Routes after curn might have to be moved
            to_move = curn->after;
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
            to_move = curn->before;
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
        else//Within
        {
	    abort();
        }
        assert_prefix(newn);
        if (to_move != NULL)
        {
            insert_subtree(parent_ptr, to_move);
        }
        assert_prefix(newn);
    }
    if (!option_f)
    {
	struct prefix *root = *parent_ptr;
	int balance = depth(root->before, root->pfxlen) -
		      depth(root->after, root->pfxlen);
	assert(balance >= -1 && balance <= 1);
	(void)balance;
    }
}

#if 0
__attribute_noinline__
static struct prefix *
find_lpm(const struct prefix *node, uint32_t key, struct prefix *lpm)
{
    while (LIKELY(node != NULL))
    {
	struct prefix *before = node->before;
	struct prefix *after = node->after;
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
	    lpm = (struct prefix *)node;
	    node = node->within;
	}
    }
    return lpm;
}
#endif

static int
compare_random(const void *a, const void *b)
{
    const struct prefix *pa = *(const struct prefix **)a;
    const struct prefix *pb = *(const struct prefix **)b;
    return pa->random - pb->random;
}

static int
compare_prefixes(const void *a, const void *b)
{
    const struct prefix *pa = *(const struct prefix **)a;
    const struct prefix *pb = *(const struct prefix **)b;
    if (pa->pfxlen != pb->pfxlen)
    {
        return (int)pa->pfxlen - (int)pb->pfxlen;
    }
    return (int64_t)pa->pfx - (int64_t)pb->pfx;
}

static void
traverse_cb(void *arg,
            uint64_t pfx,
            uint32_t pfxlen,
            p64_mbtrie_elem_t *elem,
            uint32_t actlen)
{
    (void)pfx;
    (void)pfxlen;
    (void)elem;
    (void)actlen;
    (*(size_t *)arg)++;
}

static uint64_t
time_start(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void
time_stop(const char *msg, uint64_t start, uint32_t n)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t elapsed = ts.tv_sec * 1000000000ULL + ts.tv_nsec - start;
    printf("%s: %"PRIu64".%09"PRIu64" seconds (%u items)\n", msg, elapsed / 1000000000 , elapsed % 1000000000, n);
    if (n != 0)
    {
	elapsed /= n;
	printf("%"PRIu64" nanoseconds/item\n", elapsed);
    }
}

int main(int argc, char *argv[])
{
    bool do_random = false;
    bool use_hp = false;
    uint32_t hp_refs = 2;
#if 0
    struct prefix *root = NULL;
#endif
    const char *ribfile = NULL;
    uint32_t maxroutes = 512 * 1024;

    assert(len_from_mask(0xffffffff) == 32);
    assert(len_from_mask(0xffffff00) == 24);
    assert(len_from_mask(0xffff0000) == 16);
    assert(len_from_mask(0xff000000) == 8);
    assert(len_from_mask(0x00000000) == 0);
    assert(mask_from_len( 0) == 0x00000000);
    assert(mask_from_len( 8) == 0xff000000);
    assert(mask_from_len(16) == 0xffff0000);
    assert(mask_from_len(24) == 0xffffff00);
    assert(mask_from_len(32) == 0xffffffff);

    int c;
    while ((c = getopt(argc, argv, "fhm:r:R")) != -1)
    {
        switch (c)
        {
            case 'f' :
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
            default :
usage :
            fprintf(stderr, "Usage: route <options> <rib>\n"
                            "-f               Flatten tree\n"
                            "-h               Use hazard pointers\n"
                            "-m <maxprefixes> Maximum number of prefixes\n"
                            "-r <maxrefs>     Number of HP references\n"
                            "-R               Randomize insertion order\n");
            exit(EXIT_FAILURE);
        }
    }
    if (optind + 1 > argc)
    {
        goto usage;
    }
    ribfile = argv[optind];

    if (use_hp)
    {
	printf("Using hazard pointers (nrefs=%u) for safe memory reclamation\n",
		hp_refs);
    }
    else
    {
	printf("Using qsbr for safe memory reclamation\n");
    }

    struct prefix **prefixes = malloc(maxroutes * sizeof(struct prefix *));
    if (prefixes == NULL)
    {
        perror("malloc"), exit(EXIT_FAILURE);
    }
    FILE *file = fopen(ribfile, "r");
    if (file == NULL)
    {
        perror("fopen"), exit(EXIT_FAILURE);
    }
    uint32_t n = 0, skipped = 0;
    uint32_t prev_dest = 0, prev_l = 0;
    uint32_t seed = 242;
    while (!feof(file))
    {
        uint32_t a, b, c, d, l, e, f, g, h;
        uint32_t pfx, gway;
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
        if (fscanf(file, "%u.%u.%u.%u\n", &e, &f, &g, &h) != 4)
        {
            goto syntax_error;
        }
        gway = e << 24 | f << 16 | g << 8 | h;
	if ((pfx & ~mask_from_len(l)) != 0)
	{
	    fprintf(stderr, "Prefix %u.%u.%u.%u/%u has unused bits set\n",
		    a, b, c, d, l);
	    exit(EXIT_FAILURE);
	}
        //Assume all routes refer to remote networks
        if (pfx != prev_dest || l != prev_l)
        {
            if (n == maxroutes)
            {
                fprintf(stderr, "Too many routes\n");
                exit(EXIT_FAILURE);
            }
	    struct prefix *r = alloc_prefix(pfx, l, gway, rand_r(&seed));
	    assert(r->pfx == pfx && r->pfxlen == l);
	    prefixes[n++] = r;
            prev_dest = pfx;
            prev_l = l;
        }
        else
	{
            skipped++;
	}
    }
    fclose(file);
    printf("Read %u routes (skipped %u)\n", n, skipped);

    //Sort routes based on 1) pfxlen and 2) address
    qsort(prefixes,
	  n,
	  sizeof(struct prefix *),
	  do_random ? compare_random : compare_prefixes);

    uint64_t start;
#if 0 //Disabled due to crash when inserting prefixes in random order
    nmoved = 0;
    nrebalance = 0;
    start = time_start();
    for (uint32_t i = 0; i < n; i++)
    {
        insert_prefix(&root, prefixes[i]);
    }
    time_stop("Inserting tree", start, n);
    printf("Inserted %u routes in %s order\n", n, "increasing");
    printf("%u rebalance ops, %u routes moved\n", nrebalance, nmoved);
    assert_prefix(root);
#endif

    p64_hpdomain_t *hpd;
    p64_qsbrdomain_t *qsbrd;
    if (use_hp)
    {
	hpd = p64_hazptr_alloc(100, hp_refs);
	p64_hazptr_register(hpd);
    }
    else
    {
	qsbrd = p64_qsbr_alloc(300);
	p64_qsbr_register(qsbrd);
    }
    p64_mbtrie_t *mbt = p64_mbtrie_alloc((uint8_t []){ 24, 8, 0 },
                                         route_free,
                                         NULL,
					 use_hp ? P64_MBTRIE_F_HP : 0);
    if (mbt == NULL)
    {
	perror("malloc"), exit(EXIT_FAILURE);
    }
    start = time_start();
    for (uint32_t i = 0; i < n; i++)
    {
	struct prefix *rt = prefixes[i];
	uint64_t pfx = (uint64_t)rt->pfx << 32;
	p64_mbtrie_insert(mbt, pfx, rt->pfxlen, &rt->mbe);
    }
    time_stop("Inserting mbtrie", start, n);
    size_t npfxs = 0;
    p64_mbtrie_traverse(mbt, traverse_cb, &npfxs, false);
    printf("%zu prefixes found in mbtrie\n", npfxs);

    seed = 242;
#define NLOOKUPS 2000000
    uint32_t *addrs = malloc(NLOOKUPS * sizeof(uint32_t));
    if (addrs == NULL)
    {
	perror("malloc"), exit(EXIT_FAILURE);
    }
    for (uint32_t i = 0; i < NLOOKUPS; i++)
    {
	struct prefix *rt = prefixes[rand_r(&seed) % n];
	//Random address might actually match another route
	addrs[i] = rt->pfx + (rand_r(&seed) & ~rt->mask);
    }

#if 0
    uint32_t found0 = 0;
    start = time_start();
    for (uint32_t i = 0; i < NLOOKUPS; i++)
    {
	struct prefix *rt = find_lpm(root, addrs[i], NULL);
	if (rt != NULL)
	    found0++;
    }
    time_stop("Lookup tree", start, NLOOKUPS);
    printf("%u hits (%.1f%%)\n", found0, 100 * (float)found0 / NLOOKUPS);
#endif

    uint32_t found1 = 0;
    start = time_start();
    if (use_hp)
    {
	//Use single-key lookup which uses hazard pointers
	p64_hazardptr_t hp = P64_HAZARDPTR_NULL;
	for (uint32_t i = 0; i < NLOOKUPS; i++)
	{
	    uint64_t key = (uint64_t)addrs[i] << 32;
	    p64_mbtrie_elem_t *elem = p64_mbtrie_lookup(mbt, key, &hp);
	    if (elem != NULL)
	    {
		found1++;
		//Force a memory access from returned element
		*(volatile size_t *)&elem->refcnt;
	    }
	}
	p64_hazptr_release(&hp);
	assert(p64_hazptr_dump(stdout) == 0);
    }
    else
    {
	//Use multi-key lookup which requires application to use QSBR
	for (uint32_t i = 0; i < NLOOKUPS; )
	{
	    p64_mbtrie_elem_t *results[10];
	    uint64_t keys[10];
	    uint32_t ii = i;
	    //Keys start from most significant bit in uint64_t
	    keys[0] = (uint64_t)addrs[i++] << 32;
	    keys[1] = (uint64_t)addrs[i++] << 32;
	    keys[2] = (uint64_t)addrs[i++] << 32;
	    keys[3] = (uint64_t)addrs[i++] << 32;
	    keys[4] = (uint64_t)addrs[i++] << 32;
	    keys[5] = (uint64_t)addrs[i++] << 32;
	    keys[6] = (uint64_t)addrs[i++] << 32;
	    keys[7] = (uint64_t)addrs[i++] << 32;
	    keys[8] = (uint64_t)addrs[i++] << 32;
	    keys[9] = (uint64_t)addrs[i++] << 32;
	    unsigned long res = p64_mbtrie_lookup_vec(mbt, i - ii, keys, results);
	    found1 += __builtin_popcountl(res);
	    while (res != 0)
	    {
		uint32_t j = __builtin_ctzl(res);
		res &= ~(1UL << j);
		//Force a memory access from returned element
		*(volatile size_t *)&results[j]->refcnt;
	    }
	}
	p64_qsbr_quiescent();
    }
    time_stop("Lookup mbtrie", start, NLOOKUPS);
    printf("%u hits (%.1f%%)\n", found1, 100 * (float)found1 / NLOOKUPS);

    start = time_start();
    for (uint32_t i = 0; i < n; i++)
    {
	struct prefix *rt = prefixes[i];
	uint64_t pfx = (uint64_t)rt->pfx << 32;
	p64_mbtrie_remove(mbt, pfx, rt->pfxlen, &rt->mbe, NULL);
    }
    time_stop("Removing mbtrie", start, n);
    npfxs = 0;
    p64_mbtrie_traverse(mbt, traverse_cb, &npfxs, false);
    printf("%zu prefixes found in mbtrie\n", npfxs);
    p64_mbtrie_free(mbt);

    if (use_hp)
    {
	p64_hazptr_dump(stdout);
	p64_hazptr_reclaim();
	p64_hazptr_unregister();
	p64_hazptr_free(hpd);
    }
    else
    {
	p64_qsbr_quiescent();//No references to shared objects
	p64_qsbr_reclaim();//Reclaim (free) any pending removed objects
	p64_qsbr_unregister();
	p64_qsbr_free(qsbrd);
    }
    free(addrs);
    for(uint32_t i = 0; i < n; i++)
    {
	free(prefixes[i]);
    }
    free(prefixes);
    return 0;
}
