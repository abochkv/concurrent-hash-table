#include "hash_table.h"

#include <pthread.h>
#include <stdlib.h>

typedef struct hash_node {
    int32_t key;
    struct hash_node *next;
} hash_node_t;

struct hash_bucket {
    pthread_rwlock_t lock;
    hash_node_t *head;
};

// source: https://nullprogram.com/blog/2018/07/31/
static uint32_t hash_key(int32_t key) {
    uint32_t value = (uint32_t)key;
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

int ht_init(hash_table_t *table, size_t bucket_count) {
    size_t i;

    if (table == NULL || bucket_count == 0 ||
        bucket_count > SIZE_MAX / sizeof(*table->buckets)) {
        return -1;
    }
    table->buckets = calloc(bucket_count, sizeof(*table->buckets));
    if (table->buckets == NULL) {
        return -1;
    }
    table->bucket_count = bucket_count;
    for (i = 0; i < bucket_count; ++i) {
        if (pthread_rwlock_init(&table->buckets[i].lock, NULL) != 0) {
            while (i > 0) {
                --i;
                pthread_rwlock_destroy(&table->buckets[i].lock);
            }
            free(table->buckets);
            table->buckets = NULL;
            table->bucket_count = 0;
            return -1;
        }
    }
    return 0;
}

void ht_destroy(hash_table_t *table) {
    size_t i;

    if (table == NULL || table->buckets == NULL) {
        return;
    }
    for (i = 0; i < table->bucket_count; ++i) {
        hash_node_t *node = table->buckets[i].head;
        while (node != NULL) {
            hash_node_t *next = node->next;
            free(node);
            node = next;
        }
        pthread_rwlock_destroy(&table->buckets[i].lock);
    }
    free(table->buckets);
    table->buckets = NULL;
    table->bucket_count = 0;
}

size_t ht_bucket_for(const hash_table_t *table, int32_t key) {
    return (size_t)(hash_key(key) % table->bucket_count);
}

ht_result_t ht_put(hash_table_t *table, int32_t key) {
    hash_node_t *node;
    struct hash_bucket *bucket;

    if (table == NULL || table->buckets == NULL) {
        return HT_ERROR;
    }
    bucket = &table->buckets[ht_bucket_for(table, key)];
    if (pthread_rwlock_wrlock(&bucket->lock) != 0) {
        return HT_ERROR;
    }
    for (node = bucket->head; node != NULL; node = node->next) {
        if (node->key == key) {
            pthread_rwlock_unlock(&bucket->lock);
            return HT_SUCCESS;
        }
    }
    node = malloc(sizeof(*node));
    if (node == NULL) {
        pthread_rwlock_unlock(&bucket->lock);
        return HT_ERROR;
    }
    node->key = key;
    node->next = bucket->head;
    bucket->head = node;
    pthread_rwlock_unlock(&bucket->lock);
    return HT_SUCCESS;
}

ht_result_t ht_exists(hash_table_t *table, int32_t key, bool *exists) {
    hash_node_t *node;
    struct hash_bucket *bucket;

    if (table == NULL || table->buckets == NULL || exists == NULL) {
        return HT_ERROR;
    }
    *exists = false;
    bucket = &table->buckets[ht_bucket_for(table, key)];
    if (pthread_rwlock_rdlock(&bucket->lock) != 0) {
        return HT_ERROR;
    }
    for (node = bucket->head; node != NULL; node = node->next) {
        if (node->key == key) {
            *exists = true;
            break;
        }
    }
    pthread_rwlock_unlock(&bucket->lock);
    return HT_SUCCESS;
}

ht_result_t ht_delete(hash_table_t *table, int32_t key) {
    hash_node_t **link;
    struct hash_bucket *bucket;

    if (table == NULL || table->buckets == NULL) {
        return HT_ERROR;
    }
    bucket = &table->buckets[ht_bucket_for(table, key)];
    if (pthread_rwlock_wrlock(&bucket->lock) != 0) {
        return HT_ERROR;
    }
    link = &bucket->head;
    while (*link != NULL && (*link)->key != key) {
        link = &(*link)->next;
    }
    if (*link == NULL) {
        pthread_rwlock_unlock(&bucket->lock);
        return HT_SUCCESS;
    }
    {
        hash_node_t *removed = *link;
        *link = removed->next;
        free(removed);
    }
    pthread_rwlock_unlock(&bucket->lock);
    return HT_SUCCESS;
}

int ht_test_get_page(hash_table_t *table, size_t bucket_index, size_t offset,
                     int32_t *items, size_t capacity, ht_page_t *page) {
    hash_node_t *node;
    size_t total = 0;
    size_t returned = 0;
    struct hash_bucket *bucket;

    if (table == NULL || table->buckets == NULL || page == NULL ||
        bucket_index >= table->bucket_count || (capacity > 0 && items == NULL)) {
        return -1;
    }
    bucket = &table->buckets[bucket_index];
    if (pthread_rwlock_rdlock(&bucket->lock) != 0) {
        return -1;
    }
    for (node = bucket->head; node != NULL; node = node->next) {
        if (total >= offset && returned < capacity) {
            items[returned++] = node->key;
        }
        ++total;
    }
    pthread_rwlock_unlock(&bucket->lock);
    page->total_items = total;
    page->returned_items = returned;
    return 0;
}
