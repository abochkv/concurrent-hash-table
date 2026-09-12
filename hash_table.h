#ifndef HASH_TABLE_H
#define HASH_TABLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct hash_table hash_table_t;

typedef enum {
    HT_SUCCESS,
    HT_ERROR
} ht_result_t;

typedef struct {
    size_t total_items;
    size_t returned_items;
} ht_page_t;

int ht_init(hash_table_t *table, size_t bucket_count);
void ht_destroy(hash_table_t *table);
size_t ht_bucket_for(const hash_table_t *table, int32_t key);
ht_result_t ht_put(hash_table_t *table, int32_t key);
ht_result_t ht_exists(hash_table_t *table, int32_t key, bool *exists);
ht_result_t ht_delete(hash_table_t *table, int32_t key);
int ht_test_get_page(hash_table_t *table, size_t bucket_index, size_t offset,
                     int32_t *items, size_t capacity, ht_page_t *page);

struct hash_table {
    struct hash_bucket *buckets;
    size_t bucket_count;
};

#endif
