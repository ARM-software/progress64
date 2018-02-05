#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "p64_timer.h"
#include "expect.h"

void callback(p64_timer_t tim, p64_tick_t tmo, void *arg)
{
    p64_tick_t tck = p64_timer_tick_get();
    printf("Timer %d expiration %#lx now %#lx\n", tim, tmo, tck);
    *(p64_tick_t *)arg = tck;
}

int main()
{
    p64_tick_t exp_a = -1;
    p64_timer_t tim_a = p64_timer_alloc(callback, &exp_a);
    EXPECT_F(tim_a != P64_TIMER_NULL)
    EXPECT_F(p64_timer_set(tim_a, 1));
    EXPECT_F(!p64_timer_set(tim_a, 1));
    p64_timer_tick_set(0);
    p64_timer_expire();
    EXPECT_F(exp_a == -1);
    p64_timer_tick_set(1);
    p64_timer_expire();
    EXPECT_F(exp_a == 1);
    EXPECT_F(p64_timer_set(tim_a, 2));
    EXPECT_F(p64_timer_reset(tim_a, 3))
    p64_timer_tick_set(2);
    p64_timer_expire();
    EXPECT_F(exp_a == 1);
    EXPECT_F(p64_timer_cancel(tim_a))
    p64_timer_tick_set(3);
    p64_timer_expire();
    EXPECT_F(exp_a == 1);
    EXPECT_F(!p64_timer_reset(tim_a, 0xFFFFFFFFFFFFFFFEULL));
    EXPECT_F(p64_timer_set(tim_a, 0xFFFFFFFFFFFFFFFEULL));
    EXPECT_F(p64_timer_reset(tim_a, 0xFFFFFFFFFFFFFFFEULL));
    p64_timer_expire();
    EXPECT_F(exp_a == 1);
    p64_timer_tick_set(0xFFFFFFFFFFFFFFFEULL);
    p64_timer_expire();
    EXPECT_F(exp_a == 0xFFFFFFFFFFFFFFFEULL);

    return 0;
}
