PROGRESS64
==============

Purpose
--------------
PROGRESS64 is a C library of scalable functions for concurrent programs. It
provides functionality which is often required in multithreaded networking
applications. The general goal is to provide primitives which enable
scalable and preferably non-blocking concurrent applications.

Functionality
-------------
* antireplay - replay protection (lock-free/wait-free)
* barrier - thread barrier (blocking)
* clhlock - CLH queue lock (blocking)
* hashtable - hash table (lock-free)
* hazardptr - hazard pointers (lock-free)
* laxrob - lax reorder buffer (non-blocking)
* reassemble - IP reassembly (lock-free)
* reorder - strict reorder buffer (non-blocking)
* ringbuf - SP/MP/SC/MC/LFC ring buffer (MP/MC blocking, SP/SC/LFC lock-free)
* rwlock - reader/writer lock (blocking)
* rwsync - lightweight reader/writer synchronisation (blocking)
* spinlock - basic CAS-based spin lock (blocking)
* timer - timers (lock-free)

Requirements
--------------
* A C compiler (e.g. GCC) which supports the '__atomic' builtins and inline assembler

Usage
--------------
Use library through the provided C header files. Or copy source files into
your own project.

Restrictions
--------------
PROGRESS64 currently only supports ARMv8/AArch64 and x86-64 architectures.
Several functions require 64-bit and 128-bit atomics (e.g. CAS) support in the hardware.

TODO
--------------
* Some missing examples
* Multithreaded test programs

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
