// Copyright (c) 2019 ARM Limited. All rights reserved.
//
// SPDX-License-Identifier: BSD-3-Clause

//Reader/writer version of CLH queue lock
//Optional sleep (yield to OS) after configurable timeout

#ifndef _P64_RWCLHLOCK
#define _P64_RWCLHLOCK

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct p64_rwclhnode p64_rwclhnode_t;

typedef struct
{
    p64_rwclhnode_t *tail;
    uint32_t spin_tmo;
} p64_rwclhlock_t;

#define P64_RWCLHLOCK_SPIN_FOREVER (~(uint32_t)0)

//Initialise a reader/writer CLH lock
//Specify spin timeout (in nanoseconds), after spinning for this amount of time,
//the thread will sleep (yield to the OS) until woken up
//Note that actual resolution is platform specific
void p64_rwclhlock_init(p64_rwclhlock_t *lock, uint32_t spin_tmo_ns);

//Finish a reader/writer CLH lock
void p64_rwclhlock_fini(p64_rwclhlock_t *lock);

//Acquire a reader/writer CLH lock
//*nodep will be written with a pointer to a p64_rwclhnode_t, this object must
//eventually be freed
void p64_rwclhlock_acquire_rd(p64_rwclhlock_t *lock, p64_rwclhnode_t **nodep);
void p64_rwclhlock_acquire_wr(p64_rwclhlock_t *lock, p64_rwclhnode_t **nodep);

//Release a reader/writer CLH lock
void p64_rwclhlock_release_rd(p64_rwclhnode_t **nodep);
void p64_rwclhlock_release_wr(p64_rwclhnode_t **nodep);

#ifdef __cplusplus
}
#endif

#endif
