//Copyright (c) 2025, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

//Lock-free linked list
//See "A Pragmatic Implementation of Non-Blocking Linked-Lists" by Harris

#ifndef P64_LINKLIST_H
#define P64_LINKLIST_H

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

//Flags returned from traverse callback function
#define P64_LINKLIST_F_STOP   0x01  //Stop traversal
#define P64_LINKLIST_F_REMOVE 0x02  //Remove current element
#define P64_LINKLIST_F_RETURN 0x04  //Return current element (default return NULL)

typedef uint32_t (*p64_linklist_trav_cb)(const void *arg, const p64_linklist_t *elem);

//Initialise a linked list
void p64_linklist_init(p64_linklist_t *list);

//Insert an element 'elem' after the specified list element 'prev'
//The start of the list is required in case the 'prev' element has been removed
void p64_linklist_insert(p64_linklist_t *list, p64_linklist_t *prev, p64_linklist_t *elem);

//Remove specified element from the list
void p64_linklist_remove(p64_linklist_t *list, p64_linklist_t *elem);

//Traverse linked list, calling user-defined call-back for every element
p64_linklist_t *p64_linklist_traverse(p64_linklist_t *list, p64_linklist_trav_cb cb, const void *arg);

#ifdef __cplusplus
}
#endif

#endif
