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

typedef enum
{
    p64_ll_success,  //Insert/remove/cursor-next success
    p64_ll_notfound, //Element not found in list (already removed)
    p64_ll_predmark, //Predecessor marked for removal
    p64_ll_progerror //Programming error (ignored by error handler)
} p64_linklist_status_t;

//Each element in the linked list must include a p64_linklist_t field
//A linked list starts with a dummy element
typedef struct p64_linklist
{
    struct p64_linklist *next;
} p64_linklist_t;

typedef struct
{
    p64_linklist_t *curr;
} p64_linklist_cursor_t;

//Initialise a linked list
void p64_linklist_init(p64_linklist_t *list);

//Initialise cursor with start of list
void p64_linklist_cursor_init(p64_linklist_cursor_t *curs, p64_linklist_t *list);

//Update cursor with next element in list
p64_linklist_status_t p64_linklist_cursor_next(p64_linklist_cursor_t *curs);

//Insert element 'elem' after the specified list element 'pred'
p64_linklist_status_t p64_linklist_insert(p64_linklist_t *pred, p64_linklist_t *elem);

//Remove element 'elem' from the list where 'pred' is the predecessor
p64_linklist_status_t p64_linklist_remove(p64_linklist_t *pred, p64_linklist_t *elem);

#ifdef __cplusplus
}
#endif

#endif
