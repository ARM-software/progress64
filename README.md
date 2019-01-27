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
* hazardptr - MT-safe memory reclamation (lock-free)
* laxrob - 'lax' reorder buffer (non-blocking)
* lfring - ring buffer (lock-free)
* reassemble - IP reassembly (lock-free)
* reorder - 'strict' reorder buffer (non-blocking)
* ringbuf - classic ring buffer (MP/MC blocking, lock-free dequeue)
* rwlock - reader/writer lock (blocking)
* rwsync - lightweight reader/writer synchronisation 'seqlock' (blocking)
* spinlock - basic CAS-based spin lock (blocking)
* timer - timers (lock-free)

("non-blocking" here means no thread will block (spin) but not lock-free in the academic sense, instead operations may fail early)

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

Notes
--------------
lfring is an experimental lock-free ring buffer. It needs more analysis and verification.

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
