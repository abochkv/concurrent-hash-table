#include "hash_table.h"
#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    cht_shared_memory_t *shared;
    hash_table_t *table;
} worker_context_t;

static volatile sig_atomic_t stop_requested;

static void usage(const char *program) {
    fprintf(stderr, "Usage: %s <bucket-count> [worker-count] [shm-name]\n", program);
}

static int valid_shm_name(const char *name) {
    return name != NULL && name[0] == '/' && name[1] != '\0' &&
           strchr(name + 1, '/') == NULL && strlen(name) < NAME_MAX;
}

static int parse_positive_size(const char *text, size_t *value) {
    char *end = NULL;
    unsigned long long parsed;

    if (text == NULL || *text == '\0') {
        return -1;
    }
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0' || parsed == 0 || parsed > SIZE_MAX) {
        return -1;
    }
    *value = (size_t)parsed;
    return 0;
}

static int initialize_shared(cht_shared_memory_t *shared, size_t bucket_count) {
    pthread_mutexattr_t mutex_attr;
    pthread_condattr_t cond_attr;
    unsigned int i;

    memset(shared, 0, sizeof(*shared));
    if (pthread_mutexattr_init(&mutex_attr) != 0 ||
        pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED) != 0 ||
        pthread_mutex_init(&shared->mutex, &mutex_attr) != 0) {
        return -1;
    }
    pthread_mutexattr_destroy(&mutex_attr);
    if (pthread_condattr_init(&cond_attr) != 0 ||
        pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED) != 0 ||
        pthread_cond_init(&shared->work_available, &cond_attr) != 0 ||
        pthread_cond_init(&shared->space_available, &cond_attr) != 0) {
        return -1;
    }
    for (i = 0; i < CHT_SLOT_COUNT; ++i) {
        if (pthread_cond_init(&shared->slots[i].response_ready, &cond_attr) != 0) {
            return -1;
        }
    }
    pthread_condattr_destroy(&cond_attr);
    shared->bucket_count = (uint32_t)bucket_count;
    return 0;
}

static void stop_workers(cht_shared_memory_t *shared) {
    unsigned int i;

    stop_requested = 1;
    pthread_mutex_lock(&shared->mutex);
    for (i = 0; i < shared->queue_count; ++i) {
        uint32_t slot_index = shared->queue[(shared->queue_head + i) % CHT_SLOT_COUNT];
        cht_slot_t *slot = &shared->slots[slot_index];
        memset(&slot->reply, 0, sizeof(slot->reply));
        slot->reply.status = CHT_STATUS_SERVER_ERROR;
        slot->state = CHT_SLOT_DONE;
        pthread_cond_signal(&slot->response_ready);
    }
    shared->queue_head = 0;
    shared->queue_tail = 0;
    shared->queue_count = 0;
    pthread_cond_broadcast(&shared->work_available);
    pthread_cond_broadcast(&shared->space_available);
    pthread_mutex_unlock(&shared->mutex);
}

static void make_reply(hash_table_t *table, const cht_request_t *request,
                       cht_reply_t *reply) {
    ht_result_t result;
    ht_page_t page;
    bool exists;

    memset(reply, 0, sizeof(*reply));
    switch (request->operation) {
    case CHT_OP_PUT:
        result = ht_put(table, request->key);
        reply->status = result == HT_SUCCESS ? CHT_STATUS_OK : CHT_STATUS_SERVER_ERROR;
        break;
    case CHT_OP_DELETE:
        result = ht_delete(table, request->key);
        reply->status = result == HT_SUCCESS ? CHT_STATUS_OK : CHT_STATUS_SERVER_ERROR;
        break;
    case CHT_OP_EXISTS:
        result = ht_exists(table, request->key, &exists);
        reply->status = result != HT_SUCCESS ? CHT_STATUS_SERVER_ERROR :
                        exists ? CHT_STATUS_EXISTS : CHT_STATUS_NOT_FOUND;
        break;
    case CHT_OP_TEST_GET_BUCKET:
        if (request->bucket_index >= table->bucket_count ||
            ht_test_get_page(table, request->bucket_index, request->offset,
                             reply->items, CHT_PAGE_SIZE, &page) != 0) {
            reply->status = CHT_STATUS_SERVER_ERROR;
            break;
        }
        if (page.total_items > UINT32_MAX || page.returned_items > UINT32_MAX) {
            reply->status = CHT_STATUS_SERVER_ERROR;
            break;
        }
        reply->status = CHT_STATUS_OK;
        reply->total_items = (uint32_t)page.total_items;
        reply->returned_items = (uint32_t)page.returned_items;
        break;
    default:
        reply->status = CHT_STATUS_SERVER_ERROR;
        break;
    }
}

static void *worker_main(void *argument) {
    worker_context_t *context = argument;
    cht_shared_memory_t *shared = context->shared;

    for (;;) {
        uint32_t slot_index;
        cht_request_t request;
        cht_reply_t reply;

        pthread_mutex_lock(&shared->mutex);
        while (shared->queue_count == 0 && !stop_requested) {
            pthread_cond_wait(&shared->work_available, &shared->mutex);
        }
        if (stop_requested) {
            pthread_mutex_unlock(&shared->mutex);
            return NULL;
        }
        slot_index = shared->queue[shared->queue_head];
        shared->queue_head = (shared->queue_head + 1) % CHT_SLOT_COUNT;
        --shared->queue_count;
        request = shared->slots[slot_index].request;
        pthread_mutex_unlock(&shared->mutex);

        make_reply(context->table, &request, &reply);

        pthread_mutex_lock(&shared->mutex);
        shared->slots[slot_index].reply = reply;
        shared->slots[slot_index].state = CHT_SLOT_DONE;
        pthread_cond_signal(&shared->slots[slot_index].response_ready);
        pthread_mutex_unlock(&shared->mutex);
    }
    return NULL;
}

int main(int argc, char **argv) {
    const char *shm_name = CHT_DEFAULT_SHM;
    size_t bucket_count;
    size_t worker_count = 4;
    int shm_fd = -1;
    cht_shared_memory_t *shared = MAP_FAILED;
    hash_table_t table = {0};
    pthread_t *workers = NULL;
    worker_context_t context;
    sigset_t signals;
    int signal_number;
    size_t i;
    size_t workers_started = 0;
    int exit_code = EXIT_FAILURE;

    if (argc < 2 || argc > 4 || parse_positive_size(argv[1], &bucket_count) != 0 ||
        bucket_count > UINT32_MAX ||
        (argc >= 3 && parse_positive_size(argv[2], &worker_count) != 0) ||
        worker_count > 256 || (argc == 4 && !valid_shm_name(argv[3]))) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (argc == 4) {
        shm_name = argv[3];
    }
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &signals, NULL) != 0) {
        perror("pthread_sigmask");
        return EXIT_FAILURE;
    }
    if (ht_init(&table, bucket_count) != 0) {
        fprintf(stderr, "Could not initialize hash table.\n");
        goto cleanup;
    }
    shm_fd = shm_open(shm_name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (shm_fd < 0) {
        fprintf(stderr, "Could not create %s: %s\n", shm_name, strerror(errno));
        goto cleanup;
    }
    if (ftruncate(shm_fd, (off_t)sizeof(*shared)) != 0) {
        perror("ftruncate");
        goto cleanup;
    }
    shared = mmap(NULL, sizeof(*shared), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shared == MAP_FAILED) {
        perror("mmap");
        goto cleanup;
    }
    if (initialize_shared(shared, bucket_count) != 0) {
        fprintf(stderr, "Could not initialize process-shared synchronization.\n");
        goto cleanup;
    }
    workers = calloc(worker_count, sizeof(*workers));
    if (workers == NULL) {
        perror("calloc");
        goto cleanup;
    }
    context.shared = shared;
    context.table = &table;
    for (i = 0; i < worker_count; ++i) {
        if (pthread_create(&workers[i], NULL, worker_main, &context) != 0) {
            fprintf(stderr, "Could not start worker %zu.\n", i);
            stop_workers(shared);
            for (i = 0; i < workers_started; ++i) {
                pthread_join(workers[i], NULL);
            }
            goto cleanup;
        }
        ++workers_started;
    }
    fprintf(stderr, "Server ready: %zu buckets, %zu workers, shared memory %s\n",
            bucket_count, worker_count, shm_name);
    if (sigwait(&signals, &signal_number) != 0) {
        fprintf(stderr, "sigwait failed.\n");
        stop_workers(shared);
        for (i = 0; i < workers_started; ++i) {
            pthread_join(workers[i], NULL);
        }
        goto cleanup;
    }
    fprintf(stderr, "Received signal %d; stopping workers.\n", signal_number);
    stop_workers(shared);
    for (i = 0; i < workers_started; ++i) {
        pthread_join(workers[i], NULL);
    }
    exit_code = EXIT_SUCCESS;

cleanup:
    free(workers);
    if (shared != MAP_FAILED) {
        munmap(shared, sizeof(*shared));
    }
    if (shm_fd >= 0) {
        close(shm_fd);
        shm_unlink(shm_name);
    }
    ht_destroy(&table);
    return exit_code;
}
