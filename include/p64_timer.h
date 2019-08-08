//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _P64_TIMER_H
#define _P64_TIMER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef int32_t p64_timer_t;
#define P64_TIMER_NULL -1

typedef uint64_t p64_tick_t;
#define P64_TIMER_TICK_INVALID ~UINT64_C(0)

typedef void (*p64_timer_cb)(p64_timer_t tim, p64_tick_t tmo, void *arg);

//Allocate a timer and associate with the callback and user argument
//Return P64_TIMER_NULL if no timer available
p64_timer_t p64_timer_alloc(p64_timer_cb cb, void *arg);

//Free a timer
void p64_timer_free(p64_timer_t tim);

//Set (activate) an inactive (expired or cancelled) timer
//Return false if timer already active
bool p64_timer_set(p64_timer_t tim, p64_tick_t tmo);

//Reset an active (not yet expired) timer
//Return false if timer inactive (already expired or cancelled)
bool p64_timer_reset(p64_timer_t tim, p64_tick_t tmo);

//Cancel (deactivate) an active (not yet expired) timer
//Return false if timer inactive (already expired or cancelled)
bool p64_timer_cancel(p64_timer_t tim);

//Return current timer tick
p64_tick_t p64_timer_tick_get(void);

//Set current timer tick
void p64_timer_tick_set(p64_tick_t now);

//Expire timers <= current tick and invoke call-backs
void p64_timer_expire(void);

#ifdef __cplusplus
}
#endif

#endif
