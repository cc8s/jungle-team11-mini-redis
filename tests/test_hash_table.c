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
    ENTRY_COUNT = 5,
    COLLIDING_KEY_COUNT = 4,
    COLLISION_SEARCH_LIMIT = 10000,
};

/* FNV-1a 64bit 공개 테스트 벡터 */
static const uint64_t FNV_EMPTY_HASH = 0xcbf29ce484222325ULL;
static const uint64_t FNV_A_HASH = 0xaf63dc4c8601ec8cULL;
static const uint64_t FNV_FOOBAR_HASH = 0x85944171f73967e8ULL;

static struct str *create_from_text(const char *text)
{
    return str_create(text, strlen(text));
}

static int g_destroyed_count = 0;

static void ignore_value(void *value)
{
    (void)value;
}

static void count_destroyed_value(void *value)
{
    (void)value;
    g_destroyed_count++;
}

static void destroy_str_value(void *value)
{
    str_destroy(value);
}

/* 초기 버킷 16칸 기준으로 같은 칸에 떨어지는 키를 count 개 만든다 */
static void create_colliding_keys(struct str **keys, size_t count)
{
    size_t found = 0;
    uint64_t target_idx = 0;
    for (int i = 0; found < count && i < COLLISION_SEARCH_LIMIT; i++) {
        char buf[KEY_BUF_BYTES];
        int len = snprintf(buf, sizeof(buf), "c:%d", i);
        struct str *key = str_create(buf, (size_t)len);
        uint64_t idx = hash_table_hash(key) & (HT_INITIAL_SIZE - 1);
        if (found == 0) {
            target_idx = idx;
        }
        if (idx == target_idx) {
            keys[found++] = key;
        } else {
            str_destroy(key);
        }
    }
    CHECK(found == count);
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

static void test_set_then_find_returns_value(void)
{
    static int value = 1;
    struct hash_table *ht = hash_table_create(ignore_value);
    CHECK(hash_table_set(ht, create_from_text("k"), &value) == HT_OK);
    struct str *lookup = create_from_text("k");
    const struct ht_entry *entry = hash_table_find(ht, lookup);
    CHECK(entry != NULL);
    CHECK(entry->value == &value);
    CHECK(str_equal(entry->key, lookup));
    str_destroy(lookup);
    hash_table_destroy(ht);
}

static void test_find_on_empty_table_returns_null(void)
{
    struct hash_table *ht = hash_table_create(ignore_value);
    struct str *lookup = create_from_text("k");
    CHECK(hash_table_find(ht, lookup) == NULL);
    str_destroy(lookup);
    hash_table_destroy(ht);
}

static void test_find_missing_key_returns_null(void)
{
    static int value = 1;
    struct hash_table *ht = hash_table_create(ignore_value);
    hash_table_set(ht, create_from_text("a"), &value);
    struct str *lookup = create_from_text("b");
    CHECK(hash_table_find(ht, lookup) == NULL);
    str_destroy(lookup);
    hash_table_destroy(ht);
}

static void test_set_new_keys_increments_count(void)
{
    static int value = 1;
    struct hash_table *ht = hash_table_create(ignore_value);
    hash_table_set(ht, create_from_text("a"), &value);
    hash_table_set(ht, create_from_text("b"), &value);
    hash_table_set(ht, create_from_text("c"), &value);
    CHECK(hash_table_count(ht) == 3);
    hash_table_destroy(ht);
}

static void test_set_existing_key_overwrites_value(void)
{
    static int first = 1;
    static int second = 2;
    struct hash_table *ht = hash_table_create(ignore_value);
    hash_table_set(ht, create_from_text("k"), &first);
    CHECK(hash_table_set(ht, create_from_text("k"), &second) == HT_OK);
    struct str *lookup = create_from_text("k");
    CHECK(hash_table_find(ht, lookup)->value == &second);
    CHECK(hash_table_count(ht) == 1);
    str_destroy(lookup);
    hash_table_destroy(ht);
}

static void test_set_existing_key_destroys_old_value(void)
{
    static int first = 1;
    static int second = 2;
    g_destroyed_count = 0;
    struct hash_table *ht = hash_table_create(count_destroyed_value);
    hash_table_set(ht, create_from_text("k"), &first);
    hash_table_set(ht, create_from_text("k"), &second);
    CHECK(g_destroyed_count == 1);
    hash_table_destroy(ht);
}

static void test_set_same_value_pointer_keeps_value(void)
{
    static int value = 1;
    g_destroyed_count = 0;
    struct hash_table *ht = hash_table_create(count_destroyed_value);
    hash_table_set(ht, create_from_text("k"), &value);
    hash_table_set(ht, create_from_text("k"), &value);
    CHECK(g_destroyed_count == 0);
    struct str *lookup = create_from_text("k");
    CHECK(hash_table_find(ht, lookup)->value == &value);
    str_destroy(lookup);
    hash_table_destroy(ht);
}

static void test_overwrite_then_destroy_returns_used_bytes_to_baseline(void)
{
    size_t before_bytes = mem_used_bytes();
    struct hash_table *ht = hash_table_create(destroy_str_value);
    hash_table_set(ht, create_from_text("k"), create_from_text("v1"));
    hash_table_set(ht, create_from_text("k"), create_from_text("v2"));
    hash_table_destroy(ht);
    CHECK(mem_used_bytes() == before_bytes);
}

static void test_destroy_with_entries_calls_value_destroy_per_entry(void)
{
    static int value = 1;
    g_destroyed_count = 0;
    struct hash_table *ht = hash_table_create(count_destroyed_value);
    for (int i = 0; i < ENTRY_COUNT; i++) {
        char buf[KEY_BUF_BYTES];
        int len = snprintf(buf, sizeof(buf), "key:%d", i);
        hash_table_set(ht, str_create(buf, (size_t)len), &value);
    }
    hash_table_destroy(ht);
    CHECK(g_destroyed_count == ENTRY_COUNT);
}

static void test_destroy_with_entries_returns_used_bytes_to_baseline(void)
{
    size_t before_bytes = mem_used_bytes();
    struct hash_table *ht = hash_table_create(destroy_str_value);
    for (int i = 0; i < ENTRY_COUNT; i++) {
        char buf[KEY_BUF_BYTES];
        int len = snprintf(buf, sizeof(buf), "key:%d", i);
        hash_table_set(ht, str_create(buf, (size_t)len), create_from_text("value"));
    }
    hash_table_destroy(ht);
    CHECK(mem_used_bytes() == before_bytes);
}

static void test_colliding_keys_all_findable(void)
{
    static int values[COLLIDING_KEY_COUNT];
    struct str *keys[COLLIDING_KEY_COUNT];
    struct str *lookups[COLLIDING_KEY_COUNT];
    create_colliding_keys(keys, COLLIDING_KEY_COUNT);
    struct hash_table *ht = hash_table_create(ignore_value);
    for (size_t i = 0; i < COLLIDING_KEY_COUNT; i++) {
        lookups[i] = str_dup(keys[i]);
        hash_table_set(ht, keys[i], &values[i]);
    }
    CHECK(hash_table_count(ht) == COLLIDING_KEY_COUNT);
    bool is_all_found = true;
    for (size_t i = 0; i < COLLIDING_KEY_COUNT; i++) {
        const struct ht_entry *entry = hash_table_find(ht, lookups[i]);
        is_all_found = is_all_found && entry != NULL && entry->value == &values[i];
        str_destroy(lookups[i]);
    }
    CHECK(is_all_found);
    hash_table_destroy(ht);
}

static void test_empty_key_set_and_find(void)
{
    static int value = 1;
    struct hash_table *ht = hash_table_create(ignore_value);
    CHECK(hash_table_set(ht, create_from_text(""), &value) == HT_OK);
    struct str *lookup = create_from_text("");
    const struct ht_entry *entry = hash_table_find(ht, lookup);
    CHECK(entry != NULL && entry->value == &value);
    str_destroy(lookup);
    hash_table_destroy(ht);
}

static void test_keys_differing_by_trailing_nul_are_separate(void)
{
    static const char raw[] = {'a', '\0'};
    static int plain_value = 1;
    static int nul_value = 2;
    struct hash_table *ht = hash_table_create(ignore_value);
    hash_table_set(ht, create_from_text("a"), &plain_value);
    hash_table_set(ht, str_create(raw, sizeof(raw)), &nul_value);
    CHECK(hash_table_count(ht) == 2);
    struct str *plain = create_from_text("a");
    struct str *with_nul = str_create(raw, sizeof(raw));
    CHECK(hash_table_find(ht, plain)->value == &plain_value);
    CHECK(hash_table_find(ht, with_nul)->value == &nul_value);
    str_destroy(plain);
    str_destroy(with_nul);
    hash_table_destroy(ht);
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
    RUN(test_set_then_find_returns_value);
    RUN(test_find_on_empty_table_returns_null);
    RUN(test_find_missing_key_returns_null);
    RUN(test_set_new_keys_increments_count);
    RUN(test_set_existing_key_overwrites_value);
    RUN(test_set_existing_key_destroys_old_value);
    RUN(test_set_same_value_pointer_keeps_value);
    RUN(test_overwrite_then_destroy_returns_used_bytes_to_baseline);
    RUN(test_destroy_with_entries_calls_value_destroy_per_entry);
    RUN(test_destroy_with_entries_returns_used_bytes_to_baseline);
    RUN(test_colliding_keys_all_findable);
    RUN(test_empty_key_set_and_find);
    RUN(test_keys_differing_by_trailing_nul_are_separate);
    TEST_DONE();
    return 0;
}
