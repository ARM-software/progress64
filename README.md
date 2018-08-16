PROGRESS64
==============

Purpose
--------------
PROGRESS64 is a C library of scalable functions for concurrent programs. It
provides functionality which is often required in multithreaded networking
applications. The general goal is to provide primitives which enable
completely non-blocking concurrent applications.

Functionality
-------------
antireplay - wait-free replay protection
barrier - thread barrier
clhlock - CLH queue lock
hashtable - lock-free hash table
hazardptr - hazard pointers
laxrob - lax non-blocking reorder buffer
reassemble - lock-free IP reassembly
reorder - strict non-blocking reorder buffer
ringbuf - SP/MP/SC/MC ring buffer
rwlock - reader/writer lock
rwsync - lightweight reader/writer synchronisation
spinlock - basic CAS-based spin lock
timer - lock-free timers

Usage
--------------
Use library through the provided C header files. Or copy source files into
your own project.

Restrictions
--------------
PROGRESS64 currently only supports ARMv8/AArch64 and x86-64 architectures.
Several functions require 64-bit and 128-bit atomics (e.g. CAS) support.

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
