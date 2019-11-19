//Copyrigmbt (c) 2019, ARM Limited. All rigmbts reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "p64_errhnd.h"
#include "p64_hazardptr.h"
#include "p64_qsbr.h"
#include "p64_mbtrie.h"
#include "expect.h"

#define ALIGNMENT 64
void * p64_malloc(size_t size, size_t alignment);
void p64_mfree(void *ptr);

//mbtrie requires 1 hazard pointer per trie stride
#define NUM_HAZARD_POINTERS 2

//Size of retire buffer
#define NUM_RETIRED 10

static jmp_buf jmpbuf;
static bool use_hp;

static int
error_handler(const char *module, const char *cur_err, uintptr_t val)
{
    (void)val;
    EXPECT(strcmp(module, "mbtrie") == 0);
    const char *error[] =
    {
	"prefix too long",
#define ERR_PREFIX_TOO_LONG 1
	"prefix has unused bits set",
#define ERR_PREFIX_HAS_UNUSED_BITS_SET 2
	"null element",
#define ERR_NULL_ELEMENT 3
	"element has low bits set",
#define ERR_LOW_BITS_SET 4
	NULL
    };
    for (uint32_t i = 0; error[i] != NULL; i++)
    {
	if (strcmp(cur_err, error[i]) == 0)
	{
	    longjmp(jmpbuf, i + 1);
	}
    }
    fprintf(stderr, "mbtrie: unexpected error reported: %s\n", cur_err);
    fflush(NULL);
    abort();
}

static char *
pfx2str(char buf[80], uint64_t pfx, uint32_t pfxlen)
{
    for (uint32_t i = 0; i < pfxlen; i++)
    {
	buf[i] = '0' + ((pfx & (UINT64_C(1) << 63)) != 0);
	pfx <<= 1;
    }
    sprintf(buf + pfxlen, "/%u", pfxlen);
    return buf;
}

static uint64_t
str2pfx(const char *str, uint32_t *pfxlen)
{
    uint64_t pfx = 0;
    uint32_t pos = 63;
    while (*str == '0' || *str == '1')
    {
	if (*str == '1')
	{
	    pfx |= UINT64_C(1) << pos;
	}
	pos--;
	str++;
    }
    if (pfxlen != NULL)
    {
	if (*str == '/')
	{
	    *pfxlen = atoi(str + 1);
	}
	else
	{
	    *pfxlen = 63 - pos;
	}
    }
    return pfx;
}

struct prefix
{
    p64_mbtrie_elem_t mbe;
    uint64_t pfx;
    uint32_t pfxlen;
};

static struct prefix *
elem_alloc(const char *pfx_str)
{
    struct prefix *elem = p64_malloc(sizeof(struct prefix), ALIGNMENT);
    if (elem == NULL)
	perror("malloc"), exit(-1);
    elem->mbe.refcnt = 0;
    elem->pfx = str2pfx(pfx_str, &elem->pfxlen);
    printf("Allocating prefix %s (%p)\n", pfx_str, elem);
    return elem;
}

#define container_of(pointer, type, member) \
    ((type *)(void *)(((char *)pointer) - offsetof(type, member)))

static void
elem_free(void *arg, p64_mbtrie_elem_t *ptr)
{
    (void)arg;
    char buf[80];
    struct prefix *elem = container_of(ptr, struct prefix, mbe);
    EXPECT(elem != NULL);
    printf("Freeing prefix %s (%p)\n",
	   pfx2str(buf, elem->pfx, elem->pfxlen), elem);
    if (use_hp)
    {
	while (!p64_hazptr_retire(elem, p64_mfree)) (void)0;
    }
    else
    {
	while (!p64_qsbr_retire(elem, p64_mfree)) (void)0;
    }
}

static void
traverse_cb(void *arg,
	    uint64_t pfx,
	    uint32_t pfxlen,
	    p64_mbtrie_elem_t *elem,
	    uint32_t actlen)
{
    (*(size_t *)arg)++;
    char buf[80], buf2[80];
    printf("%s contains %s (%p)\n",
	   pfx2str(buf, pfx, pfxlen),
	   pfx2str(buf2, pfx, actlen),
	   elem);
}

static size_t
count_refs(p64_mbtrie_t *mbt)
{
    size_t nrefs = 0;
    p64_mbtrie_traverse(mbt, traverse_cb, &nrefs, true);
    return nrefs;
}

static p64_mbtrie_elem_t *
lookup(p64_mbtrie_t *mbt, uint64_t key, p64_hazardptr_t *hp)
{
    if (use_hp)
    {
	return p64_mbtrie_lookup(mbt, key, hp);
    }
    else
    {
	p64_mbtrie_elem_t *res;
	unsigned long m = p64_mbtrie_lookup_vec(mbt, 1, &key, &res);
	EXPECT((m == 0 && res == NULL) || (m == 1 && res != NULL));
	return res;
    }
}

static void
release(p64_hazardptr_t *hp)
{
    if (use_hp)
    {
	p64_hazptr_release_ro(hp);
	EXPECT(p64_hazptr_dump(stdout) == NUM_HAZARD_POINTERS);
    }
}

static void
test(bool use_hp)
{
    //Need volatile to avoid "variable might be clobbered by longjmp" warning
    p64_hpdomain_t *volatile hpd;
    p64_qsbrdomain_t *volatile qsbrd;
    if (use_hp)
    {
	hpd = p64_hazptr_alloc(NUM_RETIRED, NUM_HAZARD_POINTERS);
	EXPECT(hpd != NULL);
	p64_hazptr_register(hpd);
    }
    else
    {
	qsbrd = p64_qsbr_alloc(NUM_RETIRED);
	EXPECT(qsbrd != NULL);
	p64_qsbr_register(qsbrd);
    }
    p64_hazardptr_t hp = P64_HAZARDPTR_NULL;

    p64_mbtrie_t *mbt = p64_mbtrie_alloc((uint8_t []){ 4, 4, 0 },
					 elem_free,
					 NULL,
					 use_hp ? P64_MBTRIE_F_HP : 0);
    EXPECT(mbt != NULL);
    EXPECT(count_refs(mbt) == 0);
    p64_mbtrie_elem_t *me;
    me = lookup(mbt, str2pfx("", NULL), &hp);
    EXPECT(me == NULL);
    EXPECT(hp == P64_HAZARDPTR_NULL);

printf("Inserting h1\n");
    struct prefix *h1 = elem_alloc("10/3");
    p64_mbtrie_insert(mbt, h1->pfx, h1->pfxlen, &h1->mbe);
    EXPECT(h1->mbe.refcnt == 2);
    EXPECT(count_refs(mbt) == h1->mbe.refcnt);

    me = lookup(mbt, str2pfx("0000", NULL), &hp);
    EXPECT(me == NULL);
    me = lookup(mbt, UINT64_C(0x7fffffffffffffff), &hp);
    EXPECT(me == NULL);
    me = lookup(mbt, UINT64_C(0x8000000000000000), &hp);
    EXPECT(me == &h1->mbe);
    me = lookup(mbt, UINT64_C(0x9FFFFFFFFFFFFFFF), &hp);
    EXPECT(me == &h1->mbe);
    me = lookup(mbt, UINT64_C(0xA000000000000000), &hp);
    EXPECT(me == NULL);
    release(&hp);

printf("Inserting h2\n");
    struct prefix *h2 = elem_alloc("01001/5");
    p64_mbtrie_insert(mbt, h2->pfx, h2->pfxlen, &h2->mbe);
    EXPECT(h1->mbe.refcnt == 2);
    EXPECT(h2->mbe.refcnt == 8);
    EXPECT(count_refs(mbt) == h1->mbe.refcnt + h2->mbe.refcnt);

    me = lookup(mbt, str2pfx("0000", NULL), &hp);
    EXPECT(me == NULL);
    me = lookup(mbt, UINT64_C(0x47FF000000000000), &hp);
    EXPECT(me == NULL);
    me = lookup(mbt, UINT64_C(0x4800000000000000), &hp);
    EXPECT(me == &h2->mbe);
    me = lookup(mbt, UINT64_C(0x48FF000000000000), &hp);
    EXPECT(me == &h2->mbe);
    me = lookup(mbt, UINT64_C(0x5000000000000000), &hp);
    EXPECT(me == NULL);
    release(&hp);

printf("Inserting h3\n");
    struct prefix *h3 = elem_alloc("0100101/7");
    p64_mbtrie_insert(mbt, h3->pfx, h3->pfxlen, &h3->mbe);
    count_refs(mbt);
    EXPECT(h1->mbe.refcnt == 2);
    printf("h2->mbe.refcnt=%zu\n", h2->mbe.refcnt);
    EXPECT(h2->mbe.refcnt == 6);
    EXPECT(h3->mbe.refcnt == 2);
    EXPECT(count_refs(mbt) == h1->mbe.refcnt + h2->mbe.refcnt +
				    h3->mbe.refcnt);

    me = lookup(mbt, str2pfx("0000", NULL), &hp);
    EXPECT(me == NULL);
    me = lookup(mbt, UINT64_C(0x49FF000000000000), &hp);
    EXPECT(me == &h2->mbe);
    me = lookup(mbt, UINT64_C(0x4A00000000000000), &hp);
    EXPECT(me == &h3->mbe);
    me = lookup(mbt, UINT64_C(0x4BFF000000000000), &hp);
    EXPECT(me == &h3->mbe);
    me = lookup(mbt, UINT64_C(0x4C00000000000000), &hp);
    EXPECT(me == &h2->mbe);
    release(&hp);

printf("Inserting h4\n");
    struct prefix *h4 = elem_alloc("10");
    p64_mbtrie_insert(mbt, h4->pfx, h4->pfxlen, &h4->mbe);
    EXPECT(h1->mbe.refcnt == 2);
    EXPECT(h2->mbe.refcnt == 6);
    EXPECT(h3->mbe.refcnt == 2);
    EXPECT(h4->mbe.refcnt == 2);
    EXPECT(count_refs(mbt) == h1->mbe.refcnt + h2->mbe.refcnt +
			      h3->mbe.refcnt + h4->mbe.refcnt);

    me = lookup(mbt, str2pfx("0000", NULL), &hp);
    EXPECT(me == NULL);
    me = lookup(mbt, UINT64_C(0x7FFF000000000000), &hp);
    EXPECT(me == NULL);
    me = lookup(mbt, UINT64_C(0x8000000000000000), &hp);
    EXPECT(me == &h1->mbe);
    me = lookup(mbt, UINT64_C(0x9FFF000000000000), &hp);
    EXPECT(me == &h1->mbe);
    me = lookup(mbt, UINT64_C(0xA000000000000000), &hp);
    EXPECT(me == &h4->mbe);
    me = lookup(mbt, UINT64_C(0xBFFF000000000000), &hp);
    EXPECT(me == &h4->mbe);
    release(&hp);

printf("Inserting h5\n");
    struct prefix *h5 = elem_alloc("010010/6");
    p64_mbtrie_insert(mbt, h5->pfx, h5->pfxlen, &h5->mbe);
    EXPECT(h1->mbe.refcnt == 2);
    EXPECT(h2->mbe.refcnt == 4);
    EXPECT(h3->mbe.refcnt == 2);
    EXPECT(h4->mbe.refcnt == 2);
    EXPECT(h5->mbe.refcnt == 2);
    EXPECT(count_refs(mbt) == h1->mbe.refcnt + h2->mbe.refcnt +
			      h3->mbe.refcnt + h4->mbe.refcnt +
			      h5->mbe.refcnt);

printf("Inserting h6\n");
    struct prefix *h6 = elem_alloc("0/1");
    p64_mbtrie_insert(mbt, h6->pfx, h6->pfxlen, &h6->mbe);
    EXPECT(h1->mbe.refcnt == 2);
    EXPECT(h2->mbe.refcnt == 4);
    EXPECT(h3->mbe.refcnt == 2);
    EXPECT(h4->mbe.refcnt == 2);
    EXPECT(h5->mbe.refcnt == 2);
    EXPECT(h6->mbe.refcnt == 15);
    EXPECT(count_refs(mbt) == h1->mbe.refcnt + h2->mbe.refcnt +
			      h3->mbe.refcnt + h4->mbe.refcnt +
			      h5->mbe.refcnt + h6->mbe.refcnt);

printf("Inserting h7\n");
    struct prefix *h7 = elem_alloc("/0");
    p64_mbtrie_insert(mbt, h7->pfx, h7->pfxlen, &h7->mbe);
    EXPECT(h1->mbe.refcnt == 2);
    EXPECT(h2->mbe.refcnt == 4);
    EXPECT(h3->mbe.refcnt == 2);
    EXPECT(h4->mbe.refcnt == 2);
    EXPECT(h5->mbe.refcnt == 2);
    EXPECT(h6->mbe.refcnt == 15);
    EXPECT(h7->mbe.refcnt == 1);
    EXPECT(count_refs(mbt) == h1->mbe.refcnt + h2->mbe.refcnt +
			      h3->mbe.refcnt + h4->mbe.refcnt +
			      h5->mbe.refcnt + h6->mbe.refcnt +
			      h7->mbe.refcnt);

    if (!use_hp)
    {
	printf("Vector lookup\n");
	uint64_t keys[9];
	p64_mbtrie_elem_t *results[9];
	keys[0] = str2pfx("00111111", NULL);
	keys[1] = str2pfx("01000111", NULL);
	keys[2] = str2pfx("010010001111", NULL);
	keys[3] = str2pfx("010010111111", NULL);
	keys[4] = str2pfx("01001100", NULL);
	keys[5] = str2pfx("10011111", NULL);
	keys[6] = str2pfx("10100000", NULL);
	keys[7] = str2pfx("11000000", NULL);
	keys[8] = str2pfx("11111111", NULL);
	EXPECT(p64_mbtrie_lookup_vec(mbt, 9, keys, results) == (1UL << 9) - 1);
	EXPECT(results[0] == &h6->mbe);
	EXPECT(results[1] == &h6->mbe);
	EXPECT(results[2] == &h5->mbe);
	EXPECT(results[3] == &h3->mbe);
	EXPECT(results[4] == &h2->mbe);
	EXPECT(results[5] == &h1->mbe);
	EXPECT(results[6] == &h4->mbe);
	EXPECT(results[7] == &h7->mbe);
	EXPECT(results[8] == &h7->mbe);
    }

    size_t npfxs = 0;
    p64_mbtrie_traverse(mbt, traverse_cb, &npfxs, false);
    printf("%zu prefixes found\n", npfxs);

printf("Removing h6 (replace with h7)\n");
    p64_mbtrie_remove(mbt, h6->pfx, h6->pfxlen, &h6->mbe, &h7->mbe);
    EXPECT(h1->mbe.refcnt == 2);
    EXPECT(h2->mbe.refcnt == 4);
    EXPECT(h3->mbe.refcnt == 2);
    EXPECT(h4->mbe.refcnt == 2);
    EXPECT(h5->mbe.refcnt == 2);
    EXPECT(h6->mbe.refcnt == 0);
    EXPECT(h7->mbe.refcnt == 1);
    EXPECT(count_refs(mbt) == h1->mbe.refcnt + h2->mbe.refcnt +
			      h3->mbe.refcnt + h4->mbe.refcnt +
			      h5->mbe.refcnt + h6->mbe.refcnt +
			      h7->mbe.refcnt);
    if (use_hp)
    {
	EXPECT(p64_hazptr_dump(stdout) == NUM_HAZARD_POINTERS);
    }

printf("Removing h5 (replace with h2)\n");
    p64_mbtrie_remove(mbt, h5->pfx, h5->pfxlen, &h5->mbe, &h2->mbe);
    EXPECT(h1->mbe.refcnt == 2);
    EXPECT(h2->mbe.refcnt == 6);
    EXPECT(h3->mbe.refcnt == 2);
    EXPECT(h4->mbe.refcnt == 2);
    EXPECT(h5->mbe.refcnt == 0);
    EXPECT(h6->mbe.refcnt == 0);
    EXPECT(h7->mbe.refcnt == 1);
    EXPECT(count_refs(mbt) == h1->mbe.refcnt + h2->mbe.refcnt +
			      h3->mbe.refcnt + h4->mbe.refcnt +
			      h5->mbe.refcnt + h6->mbe.refcnt +
			      h7->mbe.refcnt);

printf("Removing h4 (replace with h7)\n");
    p64_mbtrie_remove(mbt, h4->pfx, h4->pfxlen, &h4->mbe, &h7->mbe);
    EXPECT(h1->mbe.refcnt == 2);
    EXPECT(h2->mbe.refcnt == 6);
    EXPECT(h3->mbe.refcnt == 2);
    EXPECT(h4->mbe.refcnt == 0);
    EXPECT(h5->mbe.refcnt == 0);
    EXPECT(h6->mbe.refcnt == 0);
    EXPECT(h7->mbe.refcnt == 1);
    EXPECT(count_refs(mbt) == h1->mbe.refcnt + h2->mbe.refcnt +
			      h3->mbe.refcnt + h4->mbe.refcnt +
			      h5->mbe.refcnt + h6->mbe.refcnt +
			      h7->mbe.refcnt);

printf("Removing h3 (replace with h2)\n");
    p64_mbtrie_remove(mbt, h3->pfx, h3->pfxlen, &h3->mbe, &h2->mbe);
    EXPECT(h1->mbe.refcnt == 2);
    EXPECT(h2->mbe.refcnt == 8);
    EXPECT(h3->mbe.refcnt == 0);
    EXPECT(h4->mbe.refcnt == 0);
    EXPECT(h5->mbe.refcnt == 0);
    EXPECT(h6->mbe.refcnt == 0);
    EXPECT(h7->mbe.refcnt == 1);
    EXPECT(count_refs(mbt) == h1->mbe.refcnt + h2->mbe.refcnt +
			      h3->mbe.refcnt + h4->mbe.refcnt +
			      h5->mbe.refcnt + h6->mbe.refcnt +
			      h7->mbe.refcnt);

printf("Removing h2 (replace with h7)\n");
    p64_mbtrie_remove(mbt, h2->pfx, h2->pfxlen, &h2->mbe, &h7->mbe);
    EXPECT(h1->mbe.refcnt == 2);
    EXPECT(h2->mbe.refcnt == 0);
    EXPECT(h3->mbe.refcnt == 0);
    EXPECT(h4->mbe.refcnt == 0);
    EXPECT(h5->mbe.refcnt == 0);
    EXPECT(h6->mbe.refcnt == 0);
    EXPECT(h7->mbe.refcnt == 1);
    EXPECT(count_refs(mbt) == h1->mbe.refcnt + h2->mbe.refcnt +
			      h3->mbe.refcnt + h4->mbe.refcnt +
			      h5->mbe.refcnt + h6->mbe.refcnt +
			      h7->mbe.refcnt);

printf("Removing h1 (replace with h7)\n");
    p64_mbtrie_remove(mbt, h1->pfx, h1->pfxlen, &h1->mbe, &h7->mbe);
    EXPECT(h1->mbe.refcnt == 0);
    EXPECT(h2->mbe.refcnt == 0);
    EXPECT(h3->mbe.refcnt == 0);
    EXPECT(h4->mbe.refcnt == 0);
    EXPECT(h5->mbe.refcnt == 0);
    EXPECT(h6->mbe.refcnt == 0);
    EXPECT(h7->mbe.refcnt == 1);
    EXPECT(count_refs(mbt) == h1->mbe.refcnt + h2->mbe.refcnt +
			      h3->mbe.refcnt + h4->mbe.refcnt +
			      h5->mbe.refcnt + h6->mbe.refcnt +
			      h7->mbe.refcnt);

printf("Removing h7 (replace with NULL)\n");
    p64_mbtrie_remove(mbt, h7->pfx, h7->pfxlen, &h7->mbe, NULL);
    EXPECT(h1->mbe.refcnt == 0);
    EXPECT(h2->mbe.refcnt == 0);
    EXPECT(h3->mbe.refcnt == 0);
    EXPECT(h4->mbe.refcnt == 0);
    EXPECT(h5->mbe.refcnt == 0);
    EXPECT(h6->mbe.refcnt == 0);
    EXPECT(h7->mbe.refcnt == 0);
    EXPECT(count_refs(mbt) == h1->mbe.refcnt + h2->mbe.refcnt +
			      h3->mbe.refcnt + h4->mbe.refcnt +
			      h5->mbe.refcnt + h6->mbe.refcnt +
			      h7->mbe.refcnt);

    //Negative tests, requires error handler to 'ignore' error
    printf("Negative tests\n");
    int jv;
    struct prefix *hh;
    p64_errhnd_install(error_handler);

    printf("Verify that prefix too long is detected\n");
    hh = elem_alloc("/9");
    if ((jv = setjmp(jmpbuf)) == 0)
    {
	p64_mbtrie_insert(mbt, hh->pfx, hh->pfxlen, &hh->mbe);
	EXPECT(!"p64_mbtrie_insert() unexpectedly succeeded");
    }
    //Else longjumped back from error handler
    EXPECT(jv == ERR_PREFIX_TOO_LONG);
    elem_free(NULL, &hh->mbe);

    printf("Verify that prefix has unused bits set is detected\n");
    hh = elem_alloc("11111111/6");
    if ((jv = setjmp(jmpbuf)) == 0)
    {
	p64_mbtrie_insert(mbt, hh->pfx, hh->pfxlen, &hh->mbe);
	EXPECT(!"p64_mbtrie_insert() unexpectedly succeeded");
    }
    //Else longjumped back from error handler
    EXPECT(jv == ERR_PREFIX_HAS_UNUSED_BITS_SET);
    elem_free(NULL, &hh->mbe);

    printf("Verify that NULL element is detected\n");
    if ((jv = setjmp(jmpbuf)) == 0)
    {
	p64_mbtrie_insert(mbt, hh->pfx, hh->pfxlen, NULL);
	EXPECT(!"p64_mbtrie_insert() unexpectedly succeeded");
    }
    //Else longjumped back from error handler
    EXPECT(jv == ERR_NULL_ELEMENT);

    printf("Verify that low bits set are detected\n");
    if ((jv = setjmp(jmpbuf)) == 0)
    {
	p64_mbtrie_insert(mbt, hh->pfx, hh->pfxlen, (void *)1);
	EXPECT(!"p64_mbtrie_insert() unexpectedly succeeded");
    }
    //Else longjumped back from error handler
    EXPECT(jv == ERR_LOW_BITS_SET);

    //Re-install default default error handler
    EXPECT(p64_errhnd_install(NULL) == error_handler);

    p64_mbtrie_free(mbt);

    if (use_hp)
    {
	EXPECT(p64_hazptr_dump(stdout) == NUM_HAZARD_POINTERS);
	EXPECT(p64_hazptr_reclaim() == 0);
	p64_hazptr_unregister();
	p64_hazptr_free(hpd);
    }
    else
    {
	p64_qsbr_quiescent();
	EXPECT(p64_qsbr_reclaim() == 0);
	p64_qsbr_unregister();
	p64_qsbr_free(qsbrd);
    }
}

int main(void)
{
    printf("testing mbtrie using QSBR\n");
    test(use_hp = false);
    printf("testing mbtrie using HP\n");
    test(use_hp = true);
    printf("mbtrie test complete\n");
    return 0;
}
