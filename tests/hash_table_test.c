#include "hash_table.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static void test_set_and_collisions(void) {
    hash_table_t table = {0};
    int32_t keys[3] = {0};
    int32_t candidate;
    size_t found = 0;
    size_t target;
    int32_t page_items[8];
    ht_page_t page;
    bool exists;

    assert(ht_init(&table, 2) == 0);
    target = ht_bucket_for(&table, -123);
    for (candidate = -200; candidate < 200 && found < 3; ++candidate) {
        if (ht_bucket_for(&table, candidate) == target) {
            keys[found++] = candidate;
        }
    }
    assert(found == 3);
    assert(ht_put(&table, keys[0]) == HT_SUCCESS);
    assert(ht_put(&table, keys[1]) == HT_SUCCESS);
    assert(ht_put(&table, keys[2]) == HT_SUCCESS);
    assert(ht_put(&table, keys[1]) == HT_SUCCESS);
    assert(ht_exists(&table, keys[0], &exists) == HT_SUCCESS && exists);
    assert(ht_exists(&table, -9999, &exists) == HT_SUCCESS && !exists);
    assert(ht_test_get_page(&table, target, 0, page_items, 8, &page) == 0);
    assert(page.total_items == 3 && page.returned_items == 3);
    assert(ht_test_get_page(&table, target, 1, page_items, 1, &page) == 0);
    assert(page.total_items == 3 && page.returned_items == 1);
    assert(ht_delete(&table, keys[2]) == HT_SUCCESS); /* chain head */
    assert(ht_delete(&table, keys[1]) == HT_SUCCESS); /* remaining middle/tail */
    assert(ht_delete(&table, keys[0]) == HT_SUCCESS);
    assert(ht_delete(&table, keys[0]) == HT_SUCCESS);
    assert(ht_test_get_page(&table, target, 0, page_items, 8, &page) == 0);
    assert(page.total_items == 0 && page.returned_items == 0);
    ht_destroy(&table);
}

int main(void) {
    test_set_and_collisions();
    puts("hash_table_test: passed");
    return 0;
}
