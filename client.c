#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static void print_help(void) {
    puts("Commands:");
    puts("  put <int>                 insert an integer");
    puts("  exists <int>              check whether an integer is present");
    puts("  delete <int>              remove an integer");
    puts("  test-get <bucket> [offset]  test helper: inspect a bucket");
    puts("  help");
    puts("  quit");
}

static int valid_shm_name(const char *name) {
    return name != NULL && name[0] == '/' && name[1] != '\0' &&
           strchr(name + 1, '/') == NULL && strlen(name) < NAME_MAX;
}

static int parse_int32(const char *text, int32_t *value) {
    char *end = NULL;
    long long parsed;

    errno = 0;
    parsed = strtoll(text, &end, 10);
    if (text == end || *end != '\0' || errno != 0 ||
        parsed < INT32_MIN || parsed > INT32_MAX) {
        return -1;
    }
    *value = (int32_t)parsed;
    return 0;
}

static int parse_uint32(const char *text, uint32_t *value) {
    char *end = NULL;
    unsigned long long parsed;

    if (text == NULL || *text == '-' || *text == '\0') {
        return -1;
    }
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (*end != '\0' || errno != 0 || parsed > UINT32_MAX) {
        return -1;
    }
    *value = (uint32_t)parsed;
    return 0;
}

static const char *status_text(uint32_t status) {
    switch (status) {
    case CHT_STATUS_OK: return "success";
    case CHT_STATUS_EXISTS: return "exists";
    case CHT_STATUS_NOT_FOUND: return "not found";
    case CHT_STATUS_SERVER_ERROR: return "server error";
    default: return "server error";
    }
}

static int submit(cht_shared_memory_t *shared, uint32_t operation, int32_t key,
                  uint32_t bucket_index, uint32_t offset, cht_reply_t *reply) {
    uint32_t slot_index = CHT_SLOT_COUNT;
    unsigned int i;

    if (pthread_mutex_lock(&shared->mutex) != 0) {
        return -1;
    }
    for (;;) {
        for (i = 0; i < CHT_SLOT_COUNT; ++i) {
            if (shared->slots[i].state == CHT_SLOT_FREE) {
                slot_index = i;
                break;
            }
        }
        if (slot_index != CHT_SLOT_COUNT) {
            break;
        }
        pthread_cond_wait(&shared->space_available, &shared->mutex);
    }
    {
        cht_slot_t *slot = &shared->slots[slot_index];
        memset(&slot->request, 0, sizeof(slot->request));
        slot->request.operation = operation;
        slot->request.key = key;
        slot->request.bucket_index = bucket_index;
        slot->request.offset = offset;
        slot->state = CHT_SLOT_QUEUED;
        shared->queue[shared->queue_tail] = slot_index;
        shared->queue_tail = (shared->queue_tail + 1) % CHT_SLOT_COUNT;
        ++shared->queue_count;
        pthread_cond_signal(&shared->work_available);

        while (slot->state != CHT_SLOT_DONE) {
            pthread_cond_wait(&slot->response_ready, &shared->mutex);
        }
        *reply = slot->reply;
        slot->state = CHT_SLOT_FREE;
        pthread_cond_signal(&shared->space_available);
    }
    pthread_mutex_unlock(&shared->mutex);
    return 0;
}

static void print_reply(uint32_t operation, const cht_reply_t *reply) {
    uint32_t i;

    if (operation != CHT_OP_TEST_GET_BUCKET || reply->status != CHT_STATUS_OK) {
        printf("%s\n", status_text(reply->status));
        return;
    }
    printf("bucket contains %" PRIu32 " item(s); returned %" PRIu32 ":",
           reply->total_items, reply->returned_items);
    for (i = 0; i < reply->returned_items; ++i) {
        printf(" %" PRId32, reply->items[i]);
    }
    putchar('\n');
}

int main(int argc, char **argv) {
    const char *shm_name = CHT_DEFAULT_SHM;
    int fd;
    struct stat information;
    cht_shared_memory_t *shared;
    char *line = NULL;
    size_t line_capacity = 0;

    if (argc > 2 || (argc == 2 && !valid_shm_name(argv[1]))) {
        fprintf(stderr, "Usage: %s [shm-name]\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (argc == 2) {
        shm_name = argv[1];
    }
    fd = shm_open(shm_name, O_RDWR, 0);
    if (fd < 0) {
        fprintf(stderr, "Could not open %s: %s\n", shm_name, strerror(errno));
        return EXIT_FAILURE;
    }
    if (fstat(fd, &information) != 0 || (size_t)information.st_size < sizeof(*shared)) {
        fprintf(stderr, "Shared-memory object has an unexpected size.\n");
        close(fd);
        return EXIT_FAILURE;
    }
    shared = mmap(NULL, sizeof(*shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (shared == MAP_FAILED) {
        perror("mmap");
        return EXIT_FAILURE;
    }
    printf("Connected to %s (%" PRIu32 " buckets). Type 'help' for commands.\n",
           shm_name, shared->bucket_count);
    for (;;) {
        char *save = NULL;
        char *command;
        char *argument;
        char *optional;
        char *extra;
        uint32_t operation = CHT_OP_PUT;
        int32_t key = 0;
        uint32_t bucket = 0;
        uint32_t offset = 0;
        cht_reply_t reply;

        fputs("cht> ", stdout);
        fflush(stdout);
        if (getline(&line, &line_capacity, stdin) < 0) {
            break;
        }
        line[strcspn(line, "\r\n")] = '\0';
        command = strtok_r(line, " ", &save);
        argument = strtok_r(NULL, " ", &save);
        optional = strtok_r(NULL, " ", &save);
        extra = strtok_r(NULL, " ", &save);
        if (command == NULL) {
            continue;
        }
        if (strcmp(command, "quit") == 0 || strcmp(command, "exit") == 0) {
            if (argument == NULL) {
                break;
            }
        }
        if (strcmp(command, "help") == 0) {
            if (argument == NULL) {
                print_help();
                continue;
            }
        } else if (strcmp(command, "put") == 0 || strcmp(command, "delete") == 0 ||
                   strcmp(command, "exists") == 0) {
            if (argument == NULL || optional != NULL || extra != NULL ||
                parse_int32(argument, &key) != 0) {
                puts("Usage: put <int>, exists <int>, or delete <int>");
                continue;
            }
            operation = strcmp(command, "put") == 0 ? CHT_OP_PUT :
                        strcmp(command, "exists") == 0 ? CHT_OP_EXISTS : CHT_OP_DELETE;
        } else if (strcmp(command, "test-get") == 0) {
            if (argument == NULL || extra != NULL || parse_uint32(argument, &bucket) != 0 ||
                bucket >= shared->bucket_count ||
                (optional != NULL && parse_uint32(optional, &offset) != 0)) {
                puts("Usage: test-get <bucket> [offset]");
                continue;
            }
            operation = CHT_OP_TEST_GET_BUCKET;
        } else {
            puts("Unknown command. Type 'help' for commands.");
            continue;
        }
        if (submit(shared, operation, key, bucket, offset, &reply) != 0) {
            puts("Server error.");
            break;
        }
        print_reply(operation, &reply);
    }
    free(line);
    munmap(shared, sizeof(*shared));
    return EXIT_SUCCESS;
}
