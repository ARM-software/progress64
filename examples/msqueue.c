//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>

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

static p64_msqueue_elem_t *
elem_alloc(uint32_t k)
{
    p64_msqueue_elem_t *elem = malloc(sizeof(p64_msqueue_elem_t));
    if (elem == NULL)
	perror("malloc"), exit(-1);
    elem->next.ptr = NULL;
    elem->next.tag = ~0UL;//msqueue assertion
    elem->user_data = (void *)(uintptr_t)k;
    return elem;
}

static void
test_msq(uint32_t flags)
{
    struct msqueue msq;
    p64_msqueue_elem_t *elem;
    p64_hpdomain_t *hpd = NULL;

    if (flags == P64_ABA_SMR)
    {
	hpd = p64_hazptr_alloc(10, NUM_HAZARD_POINTERS);
	EXPECT(hpd != NULL);
	p64_hazptr_register(hpd);
    }

    p64_msqueue_init(&msq.qhead, &msq.qtail, flags, elem_alloc(0xdeadbabe));
    elem = p64_msqueue_dequeue(&msq.qhead, &msq.qtail);
    EXPECT(elem == NULL);
    p64_msqueue_enqueue(&msq.qhead, &msq.qtail, elem_alloc(10));
    elem = p64_msqueue_dequeue(&msq.qhead, &msq.qtail);
    EXPECT(elem != NULL && (uintptr_t)elem->user_data == 10);
    p64_mfree(elem);
    elem = p64_msqueue_dequeue(&msq.qhead, &msq.qtail);
    EXPECT(elem == NULL);
    p64_msqueue_enqueue(&msq.qhead, &msq.qtail, elem_alloc(20));
    p64_msqueue_enqueue(&msq.qhead, &msq.qtail, elem_alloc(30));
    p64_msqueue_enqueue(&msq.qhead, &msq.qtail, elem_alloc(40));
    elem = p64_msqueue_dequeue(&msq.qhead, &msq.qtail);
    EXPECT(elem != NULL && (uintptr_t)elem->user_data == 20);
    p64_mfree(elem);
    elem = p64_msqueue_dequeue(&msq.qhead, &msq.qtail);
    EXPECT(elem != NULL && (uintptr_t)elem->user_data == 30);
    p64_mfree(elem);
    elem = p64_msqueue_dequeue(&msq.qhead, &msq.qtail);
    EXPECT(elem != NULL && (uintptr_t)elem->user_data == 40);
    p64_mfree(elem);
    elem = p64_msqueue_dequeue(&msq.qhead, &msq.qtail);
    EXPECT(elem == NULL);

    elem = p64_msqueue_fini(&msq.qhead, &msq.qtail);
    EXPECT(elem != NULL);
    p64_mfree(elem);

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
