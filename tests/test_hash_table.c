#include <stddef.h>

#include "hash_table.h"
#include "mem.h"
#include "test.h"

static void ignore_value(void *value)
{
    (void)value;
}

static void test_create_returns_zero_count(void)
{
    struct hash_table *ht = hash_table_create(ignore_value);
    CHECK(ht != NULL);
    CHECK(hash_table_count(ht) == 0);
    hash_table_destroy(ht);
}

static void test_create_allocates_initial_bucket_count(void)
{
    struct hash_table *ht = hash_table_create(ignore_value);
    CHECK(hash_table_bucket_count(ht) == HT_INITIAL_SIZE);
    hash_table_destroy(ht);
}

static void test_initial_size_is_power_of_two(void)
{
    CHECK((HT_INITIAL_SIZE & (HT_INITIAL_SIZE - 1)) == 0);
}

static void test_create_without_value_destroy_succeeds(void)
{
    struct hash_table *ht = hash_table_create(NULL);
    CHECK(ht != NULL);
    CHECK(hash_table_count(ht) == 0);
    hash_table_destroy(ht);
}

static void test_destroy_empty_table_returns_used_bytes_to_baseline(void)
{
    size_t before_bytes = mem_used_bytes();
    struct hash_table *ht = hash_table_create(ignore_value);
    CHECK(mem_used_bytes() > before_bytes);
    hash_table_destroy(ht);
    CHECK(mem_used_bytes() == before_bytes);
}

static void test_destroy_null_does_nothing(void)
{
    size_t before_bytes = mem_used_bytes();
    hash_table_destroy(NULL);
    CHECK(mem_used_bytes() == before_bytes);
}

int main(void)
{
    RUN(test_create_returns_zero_count);
    RUN(test_create_allocates_initial_bucket_count);
    RUN(test_initial_size_is_power_of_two);
    RUN(test_create_without_value_destroy_succeeds);
    RUN(test_destroy_empty_table_returns_used_bytes_to_baseline);
    RUN(test_destroy_null_does_nothing);
    TEST_DONE();
    return 0;
}
