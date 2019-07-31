//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "p64_hazardptr.h"
#include "p64_msqueue.h"
#include "expect.h"
#include "os_abstraction.h"

#define NUM_HAZARD_POINTERS 2

struct msqueue
{
    _Alignas(64) p64_ptr_tag_t qhead;
    _Alignas(64) p64_ptr_tag_t qtail;
};

static p64_msqueue_elem_t *freelist = NULL;

static p64_msqueue_elem_t *
elem_alloc(void)
{
    p64_msqueue_elem_t *elem = freelist;
    if (elem != NULL)
    {
	freelist = elem->next.ptr;
    }
    else
    {
	elem = malloc(sizeof(p64_msqueue_elem_t) + sizeof(uint32_t));
	if (elem == NULL)
	{
	    perror("malloc"), exit(-1);
	}
    }
    elem->next.ptr = NULL;
    elem->next.tag = ~0UL;//msqueue assertion
    elem->max_size = sizeof(uint32_t);
    elem->cur_size = 0;
    return elem;
}

static void
elem_free(p64_msqueue_elem_t *elem)
{
    elem->next.ptr = freelist;
    freelist = elem;
}

static void
test_msq(uint32_t flags)
{
    struct msqueue msq;
    p64_msqueue_elem_t *elem;
    p64_hpdomain_t *hpd = NULL;
    uint32_t k, sizeof_k = sizeof k;

    if (flags == P64_ABA_SMR)
    {
	hpd = p64_hazptr_alloc(10, NUM_HAZARD_POINTERS);
	EXPECT(hpd != NULL);
	p64_hazptr_register(hpd);
    }

    p64_msqueue_init(&msq.qhead, &msq.qtail, flags, elem_alloc());
    elem = p64_msqueue_dequeue(&msq.qhead, &msq.qtail, &k, &sizeof_k);
    EXPECT(elem == NULL);
    p64_msqueue_enqueue(&msq.qhead, &msq.qtail,
			elem_alloc(), &(uint32_t){10}, sizeof(uint32_t));
    elem = p64_msqueue_dequeue(&msq.qhead, &msq.qtail, &k, &sizeof_k);
    EXPECT(elem != NULL && sizeof_k == sizeof k && k == 10);
    elem_free(elem);
    elem = p64_msqueue_dequeue(&msq.qhead, &msq.qtail, &k, &sizeof_k);
    EXPECT(elem == NULL);
    p64_msqueue_enqueue(&msq.qhead, &msq.qtail,
			elem_alloc(), &(uint32_t){20}, sizeof(uint32_t));
    p64_msqueue_enqueue(&msq.qhead, &msq.qtail,
			elem_alloc(), &(uint32_t){30}, sizeof(uint32_t));
    p64_msqueue_enqueue(&msq.qhead, &msq.qtail,
			elem_alloc(), &(uint32_t){40}, sizeof(uint32_t));
    elem = p64_msqueue_dequeue(&msq.qhead, &msq.qtail, &k, &sizeof_k);
    EXPECT(elem != NULL && sizeof_k == sizeof k && k == 20);
    elem_free(elem);
    elem = p64_msqueue_dequeue(&msq.qhead, &msq.qtail, &k, &sizeof_k);
    EXPECT(elem != NULL && sizeof_k == sizeof k && k == 30);
    elem_free(elem);
    elem = p64_msqueue_dequeue(&msq.qhead, &msq.qtail, &k, &sizeof_k);
    EXPECT(elem != NULL && sizeof_k == sizeof k && k == 40);
    elem_free(elem);
    elem = p64_msqueue_dequeue(&msq.qhead, &msq.qtail, &k, &sizeof_k);
    EXPECT(elem == NULL);

    elem = p64_msqueue_fini(&msq.qhead, &msq.qtail);
    EXPECT(elem != NULL);
    elem_free(elem);

    if (flags == P64_ABA_SMR)
    {
	p64_hazptr_unregister();
	p64_hazptr_free(hpd);
    }
}

int main(void)
{
    printf("testing lock-based msqueue\n");
    test_msq(P64_ABA_LOCK);
    printf("testing tag-based msqueue\n");
    test_msq(P64_ABA_TAG);
    printf("testing smr-based msqueue\n");
    test_msq(P64_ABA_SMR);
    printf("msqueue test complete\n");
    return 0;
}
