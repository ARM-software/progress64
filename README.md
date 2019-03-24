PROGRESS64
==============

Purpose
--------------
PROGRESS64 is a C library of scalable functions for concurrent programs. It
provides functionality which is often required in multithreaded networking
applications. The general goal is to provide primitives which enable
scalable and preferably non-blocking concurrent applications.

A secondary purpose is to inform and inspire the use of the C11-based memory
model (especially Release Consistency i.e. using load-acquire/store-release)
for multithreaded programming.

Functionality
-------------
* antireplay - replay protection (lock-free/wait-free)
* barrier - thread barrier (blocking)
* clhlock - CLH queue lock (blocking)
* hashtable - hash table (lock-free)
* hazardptr - MT-safe object reclamation (reader lock-free, writer blocking)
* laxrob - 'lax' reorder buffer (?)
* lfring - ring buffer (lock-free)
* qsbr - MT-safe object reclamation (reader wait-free, writer blocking)
* reassemble - IP reassembly (lock-free)
* reorder - 'strict' reorder buffer ("lockless")
* ringbuf - classic ring buffer (blocking & "lockless", lock-free dequeue)
* rwlock - reader/writer lock (blocking)
* rwlock\_r - recursive reader/writer lock (blocking)
* rwsync - lightweight reader/writer synchronisation aka 'seqlock' (blocking)
* rwsync\_r - recursive rwsync (blocking)
* spinlock - basic CAS-based spin lock (blocking)
* timer - timers (lock-free)

"Lockless" here means that individual operations will not block (wait for other
threads) but the whole data structure is not lock-free in the academic sense
(e.g. linearizable). Example, an acquired slot in a reorder buffer must
eventually be released or the reorder buffer will fill up and later release
slots will not be retired. Acquire and release operations are lockless but the
reorder buffer as a whole is neither lock-free nor obstruction-free.

Requirements
--------------
* A C compiler (e.g. GCC) which supports the '\_\_atomic' builtins and inline assembler. A lot of other GCC'isms are used as well.

Usage
--------------
Use library through the provided C header files. Or copy source files into
your own project.

Restrictions
--------------
PROGRESS64 currently only supports ARMv8/AArch64 and x86-64 architectures.
Several functions require 64-bit and 128-bit atomics (e.g. CAS) support in the hardware.
Hazardptr and qsbr support one domain only. This simplifies the API.

Notes
--------------

TODO
--------------
* Some missing examples
* Multithreaded test programs for e.g. hash table, reassembly, reorder

License
--------------
SPDX BSD-3-Clause

Design
--------------
TODO

Author
--------------
Ola Liljedahl ola.liljedahl@arm.com

Background
--------------
Many of the solutions in PROGRESS64 were created when solving scalability
problems in the 3GPP IP Transport function and when contributing to the
OpenDataPlane project.
