#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <pthread.h>
#include <stdint.h>

#define CHT_DEFAULT_SHM "/concurrent_hash_table"
#define CHT_SLOT_COUNT 32u
#define CHT_PAGE_SIZE 64u

typedef enum {
    CHT_OP_PUT = 1,
    CHT_OP_EXISTS = 2,
    CHT_OP_DELETE = 3,
    CHT_OP_TEST_GET_BUCKET = 4
} cht_operation_t;

typedef enum {
    CHT_STATUS_OK = 0,
    CHT_STATUS_EXISTS,
    CHT_STATUS_NOT_FOUND,
    CHT_STATUS_SERVER_ERROR
} cht_status_t;

typedef enum {
    CHT_SLOT_FREE = 0,
    CHT_SLOT_QUEUED,
    CHT_SLOT_DONE
} cht_slot_state_t;

typedef struct {
    uint32_t operation;
    int32_t key;
    uint32_t bucket_index;
    uint32_t offset;
} cht_request_t;

typedef struct {
    uint32_t status;
    uint32_t total_items;
    uint32_t returned_items;
    int32_t items[CHT_PAGE_SIZE];
} cht_reply_t;

typedef struct {
    uint32_t state;
    cht_request_t request;
    cht_reply_t reply;
    pthread_cond_t response_ready;
} cht_slot_t;

/* All accesses to mutable fields below must hold mutex. */
typedef struct {
    uint32_t bucket_count;
    pthread_mutex_t mutex;
    pthread_cond_t work_available;
    pthread_cond_t space_available;
    uint32_t queue[CHT_SLOT_COUNT];
    uint32_t queue_head;
    uint32_t queue_tail;
    uint32_t queue_count;
    cht_slot_t slots[CHT_SLOT_COUNT];
} cht_shared_memory_t;

#endif
