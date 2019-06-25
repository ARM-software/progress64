//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

//Lock-free IP reassembly using bespoke hash table

#ifndef _P64_REASSEMBLE_H
#define _P64_REASSEMBLE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct p64_fragment
{
    struct p64_fragment *nextfrag;
    uint64_t hash;//Hash of <IP src, IP dst addr, IP proto, IP id>
    uint32_t arrival;//Arrival time
    uint16_t fraginfo;//Fragment info from IPv4 header (host endian)
    uint16_t len;//Length in bytes of IPv4 payload (host endian)
} p64_fragment_t;

typedef struct p64_reassemble p64_reassemble_t;

typedef void (*p64_reassemble_cb)(void *arg, p64_fragment_t *frag);

//Allocate a fragment table of 'size' slots. 'size' must be a power of two.
//Specify callbacks for complete datagrams and stale fragment lists
p64_reassemble_t *p64_reassemble_alloc(uint32_t size,
				       p64_reassemble_cb complete_cb,
				       p64_reassemble_cb stale_cb,
				       void *complete_arg,
				       void *stale_arg,
				       bool extendable);

//Free a fragment table
//Pass any remaining fragments to the stale callback
void p64_reassemble_free(p64_reassemble_t *re);

//Insert a (single) fragment, perform reassembly if possible
//Fragment fields must be properly initialised
//Any complete datagrams will be passed to the completion callback
void p64_reassemble_insert(p64_reassemble_t *re,
			   p64_fragment_t *frag);

//Expire all fragments that arrived earlier than 'time'
//Any expired fragments will be passed to the stale callback
//Reassembly may also occur due to recombination with fresh fragments
//Time comparison performed using "serial number arithmetic"
void p64_reassemble_expire(p64_reassemble_t *re,
			   uint32_t time);

//Extend the fragment table (double the size)
//Return false if not extendable, out of memory or extension currently in
//progress by other thread
bool p64_reassemble_extend(p64_reassemble_t *re);

#ifdef __cplusplus
}
#endif

#endif
