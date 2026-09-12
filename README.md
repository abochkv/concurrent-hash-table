# Concurrent Hash Table Server

A Linux demonstration of a concurrent, separate-chaining hash table accessed through POSIX shared memory. The server stores signed 32-bit integers as a set; one or more interactive clients submit requests through a shared-memory queue.

## Requirements and build

Linux with a compiler, POSIX threads, and POSIX shared memory (`/dev/shm`) is required.

```sh
make
```

Run the server with its required bucket count, then start one or more clients in other terminals:

```sh
./server 31
./client
./client
```

The optional server arguments are a worker count (default `4`) and POSIX shared-memory name (default `/concurrent_hash_table`):

```sh
./server 31 8 /my_hash_table
./client /my_hash_table
```

The client commands are:

```text
put <int>
exists <int>
delete <int>
help
quit
```

`put` and `delete` print `success` after completing; they do not report whether they changed the set. Use `exists` to check a single integer with the bucket's reader lock; it prints either `exists` or `not found`.

`test-get <bucket-index> [offset]` is an inspection helper for tests. It returns a page of up to 64 linked-list values and states the bucket's total length. Supply the next offset to page through a large bucket.

Stop the server with Ctrl-C. It sends queued requests a generic `server error`, waits for its workers to stop, and removes the shared-memory name.

## Verification

```sh
make test
make stress
make clean
```

The test target includes unit tests for collision chains and an integration test with multiple client processes. For memory or race diagnostics, rebuild and run the appropriate target, for example:

```sh
make clean && make SANITIZE=address test
make clean && make SANITIZE=thread test
```
