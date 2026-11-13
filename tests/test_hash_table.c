#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "hash_table.h"
#include "mem.h"
#include "test.h"

enum {
    SAMPLE_KEY_COUNT = 1000,
    KEY_BUF_BYTES = 32,
};

/* FNV-1a 64bit 공개 테스트 벡터 */
static const uint64_t FNV_EMPTY_HASH = 0xcbf29ce484222325ULL;
static const uint64_t FNV_A_HASH = 0xaf63dc4c8601ec8cULL;
static const uint64_t FNV_FOOBAR_HASH = 0x85944171f73967e8ULL;

static struct str *create_from_text(const char *text)
{
    return str_create(text, strlen(text));
}

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

static void test_hash_empty_key_returns_offset_basis(void)
{
    struct str *key = create_from_text("");
    CHECK(hash_table_hash(key) == FNV_EMPTY_HASH);
    str_destroy(key);
}

static void test_hash_matches_fnv1a_vectors(void)
{
    struct str *a = create_from_text("a");
    struct str *foobar = create_from_text("foobar");
    CHECK(hash_table_hash(a) == FNV_A_HASH);
    CHECK(hash_table_hash(foobar) == FNV_FOOBAR_HASH);
    str_destroy(a);
    str_destroy(foobar);
}

static void test_hash_same_key_returns_same_hash(void)
{
    struct str *first = create_from_text("user:42");
    struct str *second = create_from_text("user:42");
    CHECK(first != second);
    CHECK(hash_table_hash(first) == hash_table_hash(second));
    str_destroy(first);
    str_destroy(second);
}

static void test_hash_differs_after_nul_returns_different_hash(void)
{
    static const char raw_a[] = {'a', '\0', 'b'};
    static const char raw_b[] = {'a', '\0', 'c'};
    struct str *a = str_create(raw_a, sizeof(raw_a));
    struct str *b = str_create(raw_b, sizeof(raw_b));
    CHECK(hash_table_hash(a) != hash_table_hash(b));
    str_destroy(a);
    str_destroy(b);
}

static void test_hash_trailing_nul_returns_different_hash(void)
{
    static const char raw[] = {'a', '\0'};
    struct str *without_nul = create_from_text("a");
    struct str *with_nul = str_create(raw, sizeof(raw));
    CHECK(hash_table_hash(without_nul) != hash_table_hash(with_nul));
    str_destroy(without_nul);
    str_destroy(with_nul);
}

static void test_hash_sequential_keys_cover_all_initial_buckets(void)
{
    bool is_bucket_used[HT_INITIAL_SIZE] = {false};
    for (int i = 0; i < SAMPLE_KEY_COUNT; i++) {
        char buf[KEY_BUF_BYTES];
        int len = snprintf(buf, sizeof(buf), "key:%d", i);
        struct str *key = str_create(buf, (size_t)len);
        is_bucket_used[hash_table_hash(key) & (HT_INITIAL_SIZE - 1)] = true;
        str_destroy(key);
    }
    size_t used_count = 0;
    for (size_t i = 0; i < HT_INITIAL_SIZE; i++) {
        used_count += is_bucket_used[i] ? 1 : 0;
    }
    CHECK(used_count == HT_INITIAL_SIZE);
}

int main(void)
{
    RUN(test_create_returns_zero_count);
    RUN(test_create_allocates_initial_bucket_count);
    RUN(test_initial_size_is_power_of_two);
    RUN(test_create_without_value_destroy_succeeds);
    RUN(test_destroy_empty_table_returns_used_bytes_to_baseline);
    RUN(test_destroy_null_does_nothing);
    RUN(test_hash_empty_key_returns_offset_basis);
    RUN(test_hash_matches_fnv1a_vectors);
    RUN(test_hash_same_key_returns_same_hash);
    RUN(test_hash_differs_after_nul_returns_different_hash);
    RUN(test_hash_trailing_nul_returns_different_hash);
    RUN(test_hash_sequential_keys_cover_all_initial_buckets);
    TEST_DONE();
    return 0;
}
