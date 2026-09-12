# Design and Evaluation

## Architecture

Program consists of server, client, hash table implementation and a protocol for client-server communication. Communication is signaled to the server by process-shared conditional variables.
Only the server owns the hash-table memory. It creates a fixed-size POSIX shared-memory object containing 32 request/reply slots, a FIFO of slot indexes modeled as ring buffer, and process-shared pthread synchronization. A client claims a free slot under the shared mutex, publishes a request to the FIFO, and waits on that slot's response condition variable. A server worker dequeues a slot under the same mutex, performs the table operation after releasing it, then writes the reply and signals the client. The client copies the reply and overwrites the slot as free.

Slots transition only as `FREE -> QUEUED -> DONE -> FREE`. Dequeuing grants a worker ownership of a queued slot; the slot becomes `DONE` only after its reply is ready. The shared mutex protects every transition and all FIFO fields. This lets independent client processes safely share one bounded buffer without one client consuming another's response.

## Hash table and locking

The table uses a fixed array of buckets. Each bucket is a singly linked collision chain and has its own `pthread_rwlock_t`; the linked-list nodes remain server-private heap allocations. The hash is a 32-bit mixing function followed by modulo bucket count, so negative integer keys are supported too.

`put` and `delete` use the affected bucket's write lock. `exists` uses the affected bucket's read lock while searching its chain. The `test-get` helper uses that same read lock only while traversing a chain into a fixed reply page. Consequently, readers can overlap on one bucket and operations on different buckets do not block each other. There is deliberately no table-wide lock, resize operation, or lock ordering across buckets.

The choice of fixed bucket count avoids the global coordination and iterator invalidation required by resizing. Separate chaining makes collisions explicit and permits deletion without relocating unrelated values. Set semantics keep concurrent retries idempotent: inserting an existing key leaves the set unchanged, and clients can query its presence with `exists`.

## Locking behaviour

The shared-memory mutex is held only while shared queue or slot metadata is accessed; it is not held while a worker performs a hash-table operation or while a client waits for its reply. `pthread_cond_wait` atomically releases the mutex before sleeping and reacquires it only when the client or worker wakes. The shared mutex is required for each of these handoffs:

- A client scans for a `FREE` slot, fills its request, changes it to `QUEUED`, and appends its index to the FIFO while holding the mutex. Without it, two clients could claim the same slot or corrupt the FIFO head, tail, or count.
- A worker removes an index from the FIFO and copies the request while holding the mutex. This prevents it from seeing a partially written request and ensures exactly one worker owns each queued slot. It releases the mutex before calling the hash table, allowing other clients and workers to use the queue concurrently.
- After processing, a worker writes the reply and changes the slot to `DONE` while holding the mutex, then signals that slot's condition variable. The lock publishes the completed reply and makes the state change visible to the waiting client.
- The client wakes with the same mutex held, verifies `DONE`, copies the reply, and changes `DONE` to `FREE`. Even though the worker is finished with that slot, this lock is still necessary: other clients concurrently scan slot states for `FREE`, so an unlocked write would be a data race. Signalling `space_available` while still holding the mutex makes the newly free slot and its notification one synchronized handoff.

`work_available`, each slot's `response_ready`, and `space_available` are condition variables, not state flags. Their corresponding state predicates are `queue_count > 0`, `slot.state == DONE`, and the existence of a `FREE` slot. Each predicate is checked in a loop under the shared mutex to handle spurious wakeups and to avoid missing a change between checking the predicate and sleeping.

The hash-table locks protect different data: every bucket has its own reader-writer lock. A write lock is required for `put` and `delete` because they change linked-list pointers and can allocate or free nodes; a concurrent traversal could otherwise follow a dangling pointer or observe a broken chain. A read lock is required for `exists` and `test-get` because they traverse those same nodes while another worker may delete one. Per-bucket locks are sufficient because an operation touches exactly one bucket, so unrelated buckets do not block one another.

## Lifecycle, assumptions, and tradeoffs

The server uses `O_CREAT | O_EXCL`, so a second server cannot silently take over a live segment. SIGINT and SIGTERM are synchronously handled by the main thread: queued work receives a generic server-error response, workers are awakened and joined, and the shared-memory name is unlinked. The stop flag is private to the server process; it is not shared with clients.

The implementation targets Linux POSIX shared memory and process-shared pthread mutexes/condition variables. A client that is force-killed after claiming a slot can leave that slot unavailable until the server exits; robust client-death reclamation is outside this compact teaching implementation. The bounded queue and the 64-value `test-get` reply page cap shared-memory size and make every response fixed-layout. Bucket pagination is per-request rather than a long-lived transaction, so concurrent writes may change later pages.

## Evaluation

`make test` checks collision-chain insertion/deletion, duplicate set behavior, `exists`, negative keys, test-helper paging, request parsing, and multi-client operation. `make stress` starts eight server workers and four client processes, each mixing inserts, existence checks, and deletes across 31 buckets; it reports completion of 2,400 requests. Compare same-bucket and many-bucket runs to demonstrate the effect of per-bucket reader-writer lock granularity. AddressSanitizer, ThreadSanitizer, and Helgrind are recommended additional checks where supported.
