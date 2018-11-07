//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "p64_timer.h"
#include "build_config.h"

#include "arch.h"
#include "lockfree.h"
#include "common.h"

struct timer
{
    p64_timer_cb cb;//User-defined call-back
    void *arg;//User-defined argument to call-back
};

struct freelist
{
    struct timer *head;
    uintptr_t count;//For ABA protection
};

static struct
{
    p64_tick_t earliest ALIGNED(CACHE_LINE);
    p64_tick_t current;
    uint32_t hiwmark;
    p64_tick_t expirations[MAXTIMERS + 4] ALIGNED(CACHE_LINE);//+4 for sentinels
    struct timer timers[MAXTIMERS] ALIGNED(CACHE_LINE);
    struct freelist freelist;
} g_timer;

INIT_FUNCTION
static void
init_timers(void)
{
    g_timer.earliest = P64_TIMER_TICK_INVALID;
    g_timer.current = 0;
    g_timer.hiwmark = 0;
    for (uint32_t i = 0; i < MAXTIMERS; i++)
    {
	g_timer.expirations[i] = 0;//All timers beyond hiwmark <= now
	g_timer.timers[i].cb = NULL;
	g_timer.timers[i].arg = &g_timer.timers[i + 1];
    }
    //Ensure sentinels trigger expiration compare and loop termination
    g_timer.expirations[MAXTIMERS + 0] = 0;
    g_timer.expirations[MAXTIMERS + 1] = 0;
    g_timer.expirations[MAXTIMERS + 2] = 0;
    g_timer.expirations[MAXTIMERS + 3] = 0;
    //Last timer must end freelist
    g_timer.timers[MAXTIMERS - 1].arg = NULL;
    //Initialise head of freelist
    g_timer.freelist.head = g_timer.timers;
    g_timer.freelist.count = 0;
}

//There might be user-defined data associated with a timer
//(e.g. accessed through the user-defined argument to the call-back)
//Set (and reset) a timer has release semantics wrt this data
//Expire a timer thus needs acquire semantics
static void
expire_one_timer(p64_tick_t now,
		 p64_tick_t *ptr)
{
    p64_tick_t exp;
    do
    {
	//Explicit reloading => smaller code
	exp = __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
	if (!(exp <= now))//exp > now
	{
	    //If timer does not expire anymore it means some thread has
	    //(re-)set the timer and then also updated g_timer.earliest
	    return;
	}
    }
    while (!__atomic_compare_exchange_n(ptr,
					&exp,
					P64_TIMER_TICK_INVALID,
					/*weak=*/true,
					__ATOMIC_RELAXED,
					__ATOMIC_RELAXED));
    uint32_t tim = ptr - &g_timer.expirations[0];
    g_timer.timers[tim].cb(tim, exp, g_timer.timers[tim].arg);
}

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define W0(x) (uint64_t)((x)      )
#define W1(x) (uint64_t)((x) >> 64)
#else //Big endian
#define W0(x) (uint64_t)((x) >> 64)
#define W1(x) (uint64_t)((x)      )
#endif

//__attribute_noinline__
static p64_tick_t
scan_timers(p64_tick_t now,
	    p64_tick_t *cur,
	    p64_tick_t *top)
{
    p64_tick_t earliest = P64_TIMER_TICK_INVALID;
    __int128 *ptr = (__int128 *)cur;
    __int128 pair = *ptr++;
    //Unroll 4 times seems to give best code
    //Interleave loads and compares in order to hide load-to-use latencies
    //A64: 20 insns, ~12 cycles when up to 4 cycles of load-to-use latency
    //Sentinel will ensure we eventually terminate the loop
    for (;;)
    {
	p64_tick_t w0 = W0(pair);
	p64_tick_t w1 = W1(pair);
	pair = *ptr++;
	if (UNLIKELY(w0 <= now))
	{
	    p64_tick_t *pw0 = (p64_tick_t *)(ptr - 2);
	    if (pw0 >= top)
	    {
		break;
	    }
	    expire_one_timer(now, pw0);
	    //If timer didn't actually expire, it was reset by some thread and
	    //g_timer.earliest updated which means we don't have to include it
	    //in our update of earliest
	}
	else//'w0' > 'now'
	{
	    earliest = MIN(earliest, w0);
	}
	if (UNLIKELY(w1 <= now))
	{
	    p64_tick_t *pw1 = (p64_tick_t *)(ptr - 2) + 1;
	    if (pw1 >= top)
	    {
		break;
	    }
	    expire_one_timer(now, pw1);
	}
	else//'w1' > 'now'
	{
	    earliest = MIN(earliest, w1);
	}
	w0 = W0(pair);
	w1 = W1(pair);
	pair = *ptr++;
	if (UNLIKELY(w0 <= now))
	{
	    p64_tick_t *pw0 = (p64_tick_t *)(ptr - 2);
	    if (pw0 >= top)
	    {
		break;
	    }
	    expire_one_timer(now, pw0);
	}
	else//'w0' > 'now'
	{
	    earliest = MIN(earliest, w0);
	}
	if (UNLIKELY(w1 <= now))
	{
	    p64_tick_t *pw1 = (p64_tick_t *)(ptr - 2) + 1;
	    if (pw1 >= top)
	    {
		break;
	    }
	    expire_one_timer(now, pw1);
	}
	else//'w1' > 'now'
	{
	    earliest = MIN(earliest, w1);
	}
    }
    return earliest;
}

//Perform an atomic-min operation on g_timer.earliest
static inline void
update_earliest(p64_tick_t exp)
{
    p64_tick_t old;
    do
    {
	//Explicit reloading => smaller code
	old = __atomic_load_n(&g_timer.earliest, __ATOMIC_RELAXED);
	if (exp >= old)
	{
	    //Our expiration time is same or later => no update
	    return;
	}
	//Else our expiration time is earlier than the previous 'earliest'
    }
    while (UNLIKELY(!__atomic_compare_exchange_n(&g_timer.earliest,
						 &old,
						 exp,
						 /*weak=*/true,
						 __ATOMIC_RELEASE,
						 __ATOMIC_RELAXED)));
}

void
p64_timer_expire(void)
{
    p64_tick_t now = __atomic_load_n(&g_timer.current, __ATOMIC_RELAXED);
    p64_tick_t earliest = __atomic_load_n(&g_timer.earliest, __ATOMIC_RELAXED);
    if (earliest <= now)
    {
	//There exists at least one timer that is due for expiration
	PREFETCH_FOR_READ(       &g_timer.expirations[0]                 );
	PREFETCH_FOR_READ((char*)&g_timer.expirations[0] + 1 * CACHE_LINE);
	PREFETCH_FOR_READ((char*)&g_timer.expirations[0] + 2 * CACHE_LINE);
	PREFETCH_FOR_READ((char*)&g_timer.expirations[0] + 3 * CACHE_LINE);
	//Reset 'earliest'
	__atomic_store_n(&g_timer.earliest, P64_TIMER_TICK_INVALID,
			 __ATOMIC_RELAXED);
	//We need our g_timer.earliest reset to be visible before we start to
	//scan the timer array
	smp_fence(StoreLoad);
	//Scan expiration ticks looking for expired timers
	earliest = scan_timers(now, &g_timer.expirations[0],
			       &g_timer.expirations[g_timer.hiwmark]);
	update_earliest(earliest);
    }
    //Else no timers due for expiration
}

void
p64_timer_tick_set(p64_tick_t tck)
{
    if (tck == P64_TIMER_TICK_INVALID)
    {
	fprintf(stderr, "End of time reached\n"), abort();
    }
    p64_tick_t old = __atomic_load_n(&g_timer.current, __ATOMIC_RELAXED);
    do
    {
	if (tck <= old)
	{
	    //Time cannot run backwards
	    return;
	}
    }
    while (UNLIKELY(!__atomic_compare_exchange_n(&g_timer.current,
						 &old,//Updated on failure
						 tck,
						 /*weak=*/true,
						 __ATOMIC_RELAXED,
						 __ATOMIC_RELAXED)));
}

p64_tick_t
p64_timer_tick_get(void)
{
    return __atomic_load_n(&g_timer.current, __ATOMIC_RELAXED);
}

p64_timer_t
p64_timer_alloc(p64_timer_cb cb,
		void *arg)
{
    union
    {
	struct freelist fl;
	__int128 ui;
    } old, neu;
    do
    {
	old.fl.count = __atomic_load_n(&g_timer.freelist.count, __ATOMIC_ACQUIRE);
	//count will be read before head, torn read will be detected by CAS
	old.fl.head = __atomic_load_n(&g_timer.freelist.head, __ATOMIC_ACQUIRE);
	if (UNLIKELY(old.fl.head == NULL))
	{
	    return P64_TIMER_NULL;
	}
	neu.fl.head = old.fl.head->arg;//Dereferencing old.head => need acquire
	neu.fl.count = old.fl.count + 1;
    }
    while (UNLIKELY(!lockfree_compare_exchange_16((__int128*)&g_timer.freelist,
						  &old.ui,
						  neu.ui,
						  /*weak=*/true,
						  __ATOMIC_RELAXED,
						  __ATOMIC_RELAXED)));
    uint32_t idx = old.fl.head - g_timer.timers;
    g_timer.expirations[idx] = P64_TIMER_TICK_INVALID;
    g_timer.timers[idx].cb = cb;
    g_timer.timers[idx].arg = arg;
    //Update high watermark of allocated timers
    lockfree_fetch_umax_4(&g_timer.hiwmark, idx + 1, __ATOMIC_RELEASE);
    return idx;
}

void
p64_timer_free(p64_timer_t idx)
{
    if (UNLIKELY((uint32_t)idx >= g_timer.hiwmark))
    {
	fprintf(stderr, "Invalid timer %d\n", idx), abort();
    }
    if (__atomic_load_n(&g_timer.expirations[idx], __ATOMIC_ACQUIRE) !=
	P64_TIMER_TICK_INVALID)
    {
	fprintf(stderr, "Cannot free active timer %u\n", idx), abort();
    }
    struct timer *tim = &g_timer.timers[idx];
    union
    {
	struct freelist fl;
	__int128 ui;
    } old, neu;
    do
    {
	old.fl = g_timer.freelist;
	tim->cb = NULL;
	tim->arg = old.fl.head;
	neu.fl.head = tim;
	neu.fl.count = old.fl.count + 1;
    }
    while (UNLIKELY(!lockfree_compare_exchange_16((__int128*)&g_timer.freelist,
						  &old.ui,
						  neu.ui,
						  /*weak=*/true,
						  __ATOMIC_RELEASE,
						  __ATOMIC_RELAXED)));
}

static inline bool
update_expiration(p64_timer_t idx,
		  p64_tick_t exp,
		  bool active,
		  int mo)
{
    p64_tick_t old;
    if (UNLIKELY((uint32_t)idx >= g_timer.hiwmark))
    {
	fprintf(stderr, "Invalid timer %d\n", idx), abort();
    }
    do
    {
	//Explicit reloading => smaller code
	old = __atomic_load_n(&g_timer.expirations[idx], __ATOMIC_RELAXED);
	if (active ?
		old == P64_TIMER_TICK_INVALID ://Timer inactive/expired
		old != P64_TIMER_TICK_INVALID) //Timer already active
	{
	    return false;
	}
    }
    while (UNLIKELY(!__atomic_compare_exchange_n(&g_timer.expirations[idx],
						 &old,
						 exp,
						 /*weak=*/true,
						 mo, __ATOMIC_RELAXED)));
    if (exp != P64_TIMER_TICK_INVALID)
    {
	update_earliest(exp);
    }
    return true;
}

//Setting a timer has release order (with regards to user-defined data
//associated with the timer)
bool
p64_timer_set(p64_timer_t idx,
	      p64_tick_t exp)
{
    if (UNLIKELY(exp == P64_TIMER_TICK_INVALID))
    {
	fprintf(stderr, "Invalid expiration time %"PRIu64" for timer %d\n",
		exp, idx);
	abort();
    }
    return update_expiration(idx, exp, false, __ATOMIC_RELEASE);
}

bool
p64_timer_reset(p64_timer_t idx,
		p64_tick_t exp)
{
    if (UNLIKELY(exp == P64_TIMER_TICK_INVALID))
    {
	fprintf(stderr, "Invalid expiration time %"PRIu64" for timer %d\n",
		exp, idx);
	abort();
    }
    return update_expiration(idx, exp, true, __ATOMIC_RELEASE);
}

bool
p64_timer_cancel(p64_timer_t idx)
{
    return update_expiration(idx, P64_TIMER_TICK_INVALID, true, __ATOMIC_RELAXED);
}
