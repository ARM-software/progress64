#include <stdio.h>
#include "p64_rwlock.h"
#include "expect.h"

int main(void)
{
    p64_rwlock_t lock;
    p64_rwlock_init(&lock);
    p64_rwlock_acquire_rd(&lock);
    EXPECT(lock == 1);
    p64_rwlock_acquire_rd(&lock);
    EXPECT(lock == 2);
    p64_rwlock_release_rd(&lock);
    EXPECT(lock == 1);
    p64_rwlock_release_rd(&lock);
    EXPECT(lock == 0);
    p64_rwlock_acquire_wr(&lock);
    EXPECT(lock == 0x80000000);
    p64_rwlock_release_wr(&lock);
    EXPECT(lock == 0);

    printf("rwlock tests complete\n");
    return 0;
}
