//Copyrigmbt (c) 2020, ARM Limited. All rigmbts reserved.
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
#include "p64_mcas.h"
#include "expect.h"

#define NUM_HAZARD_POINTERS 5

//Size of retire buffer
#define NUM_RETIRED 10

static jmp_buf jmpbuf;

static int
error_handler(const char *module, const char *cur_err, uintptr_t val)
{
    (void)val;
    EXPECT(strcmp(module, "mcas") == 0);
    const char *error[] =
    {
	"duplicate address",
#define ERR_DUPLICATE_ADDRESS 1
	"invalid argument",
#define ERR_INVALID_ARGUMENT 2
	NULL
    };
    for (uint32_t i = 0; error[i] != NULL; i++)
    {
	if (strcmp(cur_err, error[i]) == 0)
	{
	    longjmp(jmpbuf, i + 1);
	}
    }
    fprintf(stderr, "mcas: unexpected error reported: %s\n", cur_err);
    fflush(NULL);
    abort();
}

struct node
{
    _Alignas(4)
    uint8_t dummy0;
    uint8_t dummy1;
};

static void
test(bool use_hp)
{
    //Need volatile to avoid "variable might be clobbered by longjmp" warning
    p64_hpdomain_t *volatile hpd;
    p64_qsbrdomain_t *volatile qsbrd;
    p64_hazardptr_t hp = P64_HAZARDPTR_NULL;
    p64_hazardptr_t *hpp0 = use_hp ? &hp : NULL;
    p64_mcas_ptr_t table[10] = { NULL };
    struct node node;
    p64_mcas_ptr_t *loc[2], exp[2], new[2];

    p64_mcas_init(4, 2);
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

    printf("Test p64_mcas_read()\n");
    EXPECT(p64_mcas_read(&table[0], hpp0) == NULL);

    printf("Test p64_mcas_cas1()\n");
    EXPECT(p64_mcas_cas1(&table[0], NULL, &node, hpp0) == true);
    EXPECT(table[0] == &node);
    EXPECT(table[1] == NULL);
    EXPECT(p64_mcas_read(&table[0], hpp0) == &node);

    printf("Test p64_mcas_casn()\n");
    loc[0] = &table[0];
    loc[1] = &table[1];
    exp[0] = &node;
    exp[1] = NULL;
    new[0] = NULL;
    new[1] = &node;
    EXPECT(p64_mcas_casn(2, loc, exp, new, use_hp) == true);
    EXPECT(table[0] == NULL);
    EXPECT(table[1] == &node);

    //Reverse address order
    loc[0] = &table[2];
    loc[1] = &table[1];
    exp[0] = NULL;
    exp[1] = &node;
    new[0] = &node;
    new[1] = NULL;
    EXPECT(p64_mcas_casn(2, loc, exp, new, use_hp) == true);
    EXPECT(table[1] == NULL);
    EXPECT(table[2] == &node);

    //Negative tests, requires error handler to 'ignore' error
    printf("Negative tests\n");
    int jv;
    p64_errhnd_install(error_handler);

    printf("Verify that duplicate addresses are detected\n");
    loc[0] = &table[2];
    loc[1] = &table[2];
    exp[0] = &node;
    exp[1] = &node;
    new[0] = NULL;
    new[1] = NULL;
    if ((jv = setjmp(jmpbuf)) == 0)
    {
	p64_mcas_casn(2, loc, exp, new, use_hp);
	EXPECT(!"p64_mcas_casn() unexpectedly succeeded");
    }
    //Else longjumped back from error handler
    EXPECT(jv == ERR_DUPLICATE_ADDRESS);

    printf("Verify that invalid pointers are detected\n");
    loc[0] = &table[2];
    loc[1] = &table[5];
    exp[0] = &node.dummy1;
    exp[1] = NULL;
    new[0] = NULL;
    new[1] = &node.dummy1;
    if ((jv = setjmp(jmpbuf)) == 0)
    {
	p64_mcas_casn(2, loc, exp, new, use_hp);
	EXPECT(!"p64_mcas_casn() unexpectedly succeeded");
    }
    //Else longjumped back from error handler
    EXPECT(jv == ERR_INVALID_ARGUMENT);

    if (use_hp)
    {
	p64_hazptr_release(&hp);
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
    p64_mcas_fini();
}

int main(void)
{
    printf("Testing mcas using QSBR\n");
    test(false);
    printf("Testing mcas using HP\n");
    test(true);
    printf("mcas tests complete\n");
    return 0;
}
