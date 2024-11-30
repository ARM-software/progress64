//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdint.h>

#include "p64_tktlock.h"
#include "build_config.h"

#include "arch.h"
#include "inline.h"
#include "common.h"

//Lock word is 32 bits
//16msb are the ticket counter
#define TKT(w) ((w) >> 16)
//16lsb are the current ticket
#define CUR(w) ((w) & 0xFFFF)
//Increment value for taking a ticket
#define TKTINC (1U << 16)

void
p64_tktlock_init(p64_tktlock_t *lock)
{
    *lock = 0;
}

void
p64_tktlock_acquire_bkoff(p64_tktlock_t *lock, uint32_t time)
{
    //Get a ticket, also read current ticket
    uint32_t word = __atomic_fetch_add(lock, TKTINC, __ATOMIC_ACQUIRE);
    uint16_t tkt = TKT(word);
    uint16_t cur = CUR(word);
    //Wait for any previous lockers to leave
    while (tkt != cur)
    {
	uint16_t dist = tkt - cur;
	if (dist == 1)
	{
	    //We are next so no back-off
#if __LITTLE_ENDIAN__ || __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	    wait_until_equal((uint16_t *)lock, tkt, __ATOMIC_ACQUIRE);
#else
#error Not a little endian system
#endif
	    return;
	}
	nano_delay((dist - 1) * time);
	word = __atomic_load_n(lock, __ATOMIC_ACQUIRE);
	cur = CUR(word);
    }
}

void
p64_tktlock_acquire(p64_tktlock_t *lock)
{
    p64_tktlock_acquire_bkoff(lock, 192);
}

void
p64_tktlock_release(p64_tktlock_t *lock)
{
    //Release lock ownership
    //Operate on cur field only to avoid overflowing into ticket counter
#if __LITTLE_ENDIAN__ || __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    //Increment cur field (16 lsb, assume little endian system)
    (void)__atomic_fetch_add((uint16_t *)lock, 1, __ATOMIC_RELEASE);
#else
#error Not a little endian system
#endif
}
