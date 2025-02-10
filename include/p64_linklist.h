//Copyright (c) 2025, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

//Lock-free linked list
//See "A Pragmatic Implementation of Non-Blocking Linked-Lists" by Harris

#ifndef P64_LINKLIST_H
#define P64_LINKLIST_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

//Each element in the linked list must include a p64_linklist_t field
//A linked list starts with a dummy element
typedef struct p64_linklist
{
    struct p64_linklist *next;
} p64_linklist_t;

//Initialise a linked list
void p64_linklist_init(p64_linklist_t *list);

//Return next (unmarked) element in list
//Unlink any encountered element marked for removal
p64_linklist_t *p64_linklist_next(p64_linklist_t *curr);

//Insert element 'elem' after the specified list element 'pred'
//Return values:
//true: success, 'elem' inserted somewhere after 'pred'
//false: failure, all potential predecessors marked for removal
bool p64_linklist_insert(p64_linklist_t *pred, p64_linklist_t *elem);

//Remove element 'elem' from the list where 'pred' is the predecessor
//Return values:
//true: 'elem' marked for removal and unlinked (by caller or other thread)
//false: 'elem' marked for removal but failed to unlink it (predecessor
//also marked for removal), traverse list to unlink any marked elements
bool p64_linklist_remove(p64_linklist_t *pred, p64_linklist_t *elem);

#ifdef __cplusplus
}
#endif

#endif
