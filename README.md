# PROGRESS64
Progress by Design
## Purpose
PROGRESS64 is a C library of scalable functions for concurrent programs. It provides functionality which is often required in multithreaded networking applications. The general goal is to provide primitives which enable scalable and preferably non-blocking concurrent applications.

A secondary purpose is to inform and inspire the use of the C11-based memory model (especially Release Consistency i.e. using load-acquire/store-release) for multithreaded programming.

## Functionality
### Non-blocking functions
| Name | Description | Properties |
| ---- | ---- | :----: |
| antireplay | replay protection | lock-free/wait-free
| buckring | ring buffer using pass-the-buck algorithm | non-blocking (1)
| buckrob | reorder buffer using pass-the-buck algorithm | non-blocking (1)
| counter | shared counters | reader obstruction-free, writer wait-free
| cuckooht | hash table - cuckoo with cellar, one-level move | non-blocking (1)
| hashtable | hash table - separate chaining with linked lists | lock-free
| hazardptr | safe object reclamation using hazard pointers | reader lock-free, writer blocking/non-blocking
| hopscotch | hash table - hopscotch with cellar | non-blocking (1)
| msqueue | Michael & Scott queue with configurable ABA workaround (lock/tag/smr) | blocking/lock-free
| laxrob | 'lax' reorder buffer | non-blocking (1)
| lfring | ring buffer | lock-free
| mbtrie | multi-bit trie | reader lock-free/wait-free, writer non-blocking (1)
| qsbr | safe object reclamation using quiescent state based reclamation | reader wait-free, writer blocking
| reassemble | IP reassembly | lock-free (2), resizeable
| reorder | 'strict' reorder buffer | non-blocking (1)
| ringbuf | classic ring buffer, support for user-defined element type | blocking & non-blocking (1), lock-free dequeue
| stack | Treiber stack with configurable ABA workaround (lock/tag/smr/llsc) | blocking/lock-free
| timer | timers | lock-free

"Obstruction-free", "lock-free" and "wait-free" have the standard definitions from computer science.

(1) Non-blocking but not (always) linearizable  
(2) Blocking (using per-bucket locks) on ARMv7ve due to missing support for 128-bit atomic operations.

### Locks & other blocking functions
| Name | Description | Properties |
| ---- | ---- | :----: |
| barrier | thread barrier | blocking |
| clhlock | CLH queue lock | mutex, fcfs, queue |
| mcslock | MCS queue lock | mutex, fcfs, queue |
| pfrwlock | phase fair reader/writer lock | rw, fcfs |
| rwclhlock | reader/writer CLH queue lock | rw, fcfs, queue, sleep |
| rwlock | reader/writer lock (writer preference) | rw |
| rwlock\_r | recursive version of rwlock | rw, recursive |
| rwsync | lightweight reader/writer synchronisation aka 'seqlock' (writer preference) | rw |
| rwsync\_r | recursive version of rwsync | rw, recursive |
| semaphore | counting semaphore | rw, fcfs |
| skiplock | skippable ticket-like lock | mutex |
| spinlock | basic CAS-based spin lock | mutex |
| tfrwlock | task fair reader/writer lock | rw, fcfs |
| tfrwlock\_r | recursive version of tfrw lock | rw, fcfs, recursive |
| tktlock | ticket lock | mutex, fcfs |

"mutex" - mutual exclusion, only one thread at a time can acquire lock.  
"rw" - multiple threads may concurrently acquire lock in reader (shared) mode.  
"fcfs" - first come, first served. FCFS locks can be considered fair.  
"queue" - each waiting thread spins on a separate location. Queue locks generally scale better with high lock contention.  
"recursive" - the same thread can re-acquire the lock when it is already acquired.  
"sleep" - waiting thread will sleep after spinning has timed out.

## Requirements
* A C11 compiler (e.g. GCC or Clang) which supports the '\_\_atomic' builtins and inline assembler. Several other GCC'isms are used as well.
* Several functions require 64-bit and 128-bit atomics (e.g. CAS) support in the hardware.

## HW/SW Support
  * Architectures
    * ARMv7ve (ARMv7a with LPAE/64-bit atomic LDRD/STRD, e.g. Cortex-A7/A15/A17; also ARMv8 in AArch32 state)
    * ARMv8/AArch64 (aka ARM64)
    * x86-64
  * Operating Systems
    * Linux
    * Windows
    * macOS (Darwin)

## Usage
Use library through the provided C header files. Or copy source files into your own project.

## Notes
* Hazardptr and QSBR support one reclamation domain only. This is a trade-off that simplifies the API and usage. For QSBR, multiple reclamation domains do make sense...
* The hazard pointer implementation is non-blocking (wait-free) when a thread has space for more retired objects than the total number of hazard pointers (for all threads).
* The hazard pointer API will actually use the QSBR implementation when 'nrefs' (number of hazard pointers per thread) is set to 0 when the hazard pointer domain is allocated.
* The resizeable reassembly function is experimental and has not yet endured stress testing.
* The mbtrie is experimental and has not yet endured stress testing.
* The hopscotch hash table is experimental and has not yet endured stress testing.
* The cuckooht hash table is experimental and has not yet endured stress testing.
* When using Safe Memory Reclamation as ABA workaround with the Treiber stack, LIFO order is not guaranteed (so not really a LIFO stack...)
* The skiplock is a simplified version of a ring buffer based ticket-like lock.

## TODO
* Some missing examples
* Multithreaded stress test programs for e.g. hash tables, reassembly, mbtrie
* Use C11 features instead of GNU extensions and other non-standard features
* Remove "_" from P64 defines

## License
SPDX BSD-3-Clause

## Design
TODO

## Author
Ola Liljedahl ola.liljedahl@arm.com

## Background
Many of the solutions in PROGRESS64 were created when solving scalability problems in the 3GPP IP Transport function and when contributing to the OpenDataPlane project.
