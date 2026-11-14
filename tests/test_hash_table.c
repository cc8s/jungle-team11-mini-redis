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
    RANDOM_TRIAL_COUNT = 1000,
    BIG_KEY_COUNT = 100000,
    KEPT_KEY_COUNT = 10,
    CHURN_KEY_COUNT = 1000,
};

/* SipHash-2-4 참조 구현의 테스트 벡터: key = 00 01 .. 0f, 메시지 = 00 01 .. (len-1) */
struct sip_vector {
    size_t len;
    uint64_t hash;
};

static const struct sip_vector SIP_VECTORS[] = {
    {0, 0x726fdb47dd0e0e31ULL}, {1, 0x74f839c593dc67fdULL},  {7, 0xab0200f58b01d137ULL},
    {8, 0x93f5f5799a932462ULL}, {15, 0xa129ca6149be45e5ULL}, {16, 0x3f2acc7f57c29bdbULL},
};

enum {
    SIP_VECTOR_COUNT = sizeof(SIP_VECTORS) / sizeof(SIP_VECTORS[0]),
    SIP_VECTOR_MAX_LEN = 16,
};

static void set_reference_seed(void)
{
    uint8_t seed[HT_SEED_BYTES];
    for (size_t i = 0; i < HT_SEED_BYTES; i++) {
        seed[i] = (uint8_t)i;
    }
    hash_table_set_seed(seed);
}

static uint64_t hash_counting_bytes(size_t len)
{
    char message[SIP_VECTOR_MAX_LEN];
    for (size_t i = 0; i < len; i++) {
        message[i] = (char)i;
    }
    struct str *key = str_create(message, len);
    uint64_t hash = hash_table_hash(key);
    str_destroy(key);
    return hash;
}

static struct str *create_from_text(const char *text)
{
    return str_create(text, strlen(text));
}

static int g_destroyed_count = 0;

static struct str *create_numbered_key(int number)
{
    char buf[KEY_BUF_BYTES];
    int len = snprintf(buf, sizeof(buf), "key:%d", number);
    return str_create(buf, (size_t)len);
}

/* key:0 ~ key:(count-1) 를 모두 value 로 저장한다 */
static void set_numbered_keys(struct hash_table *ht, int count, void *value)
{
    for (int i = 0; i < count; i++) {
        hash_table_set(ht, create_numbered_key(i), value);
    }
}

/* key:first ~ key:(last-1) 를 모두 지운다 */
static void delete_numbered_keys(struct hash_table *ht, int first, int last)
{
    for (int i = first; i < last; i++) {
        struct str *lookup = create_numbered_key(i);
        hash_table_delete(ht, lookup);
        str_destroy(lookup);
    }
}

/* key:first ~ key:(last-1) 가 모두 조회되면 true */
static bool is_all_numbered_found(const struct hash_table *ht, int first, int last)
{
    bool is_all_found = true;
    for (int i = first; i < last; i++) {
        struct str *lookup = create_numbered_key(i);
        is_all_found = is_all_found && hash_table_find(ht, lookup) != NULL;
        str_destroy(lookup);
    }
    return is_all_found;
}

static bool is_power_of_two(size_t n)
{
    return n != 0 && (n & (n - 1)) == 0;
}

static size_t run_rehash_to_end(struct hash_table *ht);

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

/* 한 체인에 충돌 키 4개를 쌓은 테이블. 맨 앞에 붙이므로 체인 순서는 삽입의 역순이다 */
struct collision_fixture {
    struct hash_table *ht;
    struct str *lookups[COLLIDING_KEY_COUNT];
    int values[COLLIDING_KEY_COUNT];
};

enum {
    CHAIN_END_IDX = 0,
    CHAIN_MIDDLE_IDX = 1,
    CHAIN_FRONT_IDX = COLLIDING_KEY_COUNT - 1,
};

static void collision_fixture_init(struct collision_fixture *fx)
{
    struct str *keys[COLLIDING_KEY_COUNT];
    create_colliding_keys(keys, COLLIDING_KEY_COUNT);
    fx->ht = hash_table_create(ignore_value);
    for (size_t i = 0; i < COLLIDING_KEY_COUNT; i++) {
        fx->lookups[i] = str_dup(keys[i]);
        fx->values[i] = (int)i;
        hash_table_set(fx->ht, keys[i], &fx->values[i]);
    }
}

static void collision_fixture_deinit(struct collision_fixture *fx)
{
    for (size_t i = 0; i < COLLIDING_KEY_COUNT; i++) {
        str_destroy(fx->lookups[i]);
    }
    hash_table_destroy(fx->ht);
}

/* missing_idx 키만 없고 나머지는 자기 값으로 찾아지면 true. 범위 밖이면 전부 있어야 한다 */
static bool is_only_missing(const struct collision_fixture *fx, size_t missing_idx)
{
    bool is_expected = true;
    for (size_t i = 0; i < COLLIDING_KEY_COUNT; i++) {
        const struct ht_entry *entry = hash_table_find(fx->ht, fx->lookups[i]);
        bool should_exist = i != missing_idx;
        bool is_found = entry != NULL && entry->value == &fx->values[i];
        is_expected = is_expected && (should_exist ? is_found : entry == NULL);
    }
    return is_expected;
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

static void test_hash_matches_siphash_vectors(void)
{
    set_reference_seed();
    bool is_all_match = true;
    for (size_t i = 0; i < SIP_VECTOR_COUNT; i++) {
        is_all_match =
            is_all_match && hash_counting_bytes(SIP_VECTORS[i].len) == SIP_VECTORS[i].hash;
    }
    CHECK(is_all_match);
}

static void test_hash_with_different_seed_returns_different_hash(void)
{
    static const uint8_t other_seed[HT_SEED_BYTES] = {1};
    struct str *key = create_from_text("user:42");
    set_reference_seed();
    uint64_t reference_hash = hash_table_hash(key);
    hash_table_set_seed(other_seed);
    CHECK(hash_table_hash(key) != reference_hash);
    str_destroy(key);
}

static void test_load_random_seed_changes_hash(void)
{
    struct str *key = create_from_text("user:42");
    set_reference_seed();
    uint64_t reference_hash = hash_table_hash(key);
    CHECK(hash_table_load_random_seed() == HT_OK);
    CHECK(hash_table_hash(key) != reference_hash);
    str_destroy(key);
}

static void test_table_created_after_random_seed_finds_keys(void)
{
    static int value = 1;
    CHECK(hash_table_load_random_seed() == HT_OK);
    struct hash_table *ht = hash_table_create(ignore_value);
    set_numbered_keys(ht, CHURN_KEY_COUNT, &value);
    CHECK(is_all_numbered_found(ht, 0, CHURN_KEY_COUNT));
    hash_table_destroy(ht);
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
    struct collision_fixture fx;
    collision_fixture_init(&fx);
    CHECK(hash_table_count(fx.ht) == COLLIDING_KEY_COUNT);
    CHECK(is_only_missing(&fx, COLLIDING_KEY_COUNT));
    collision_fixture_deinit(&fx);
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

static void test_delete_chain_front_keeps_others(void)
{
    struct collision_fixture fx;
    collision_fixture_init(&fx);
    CHECK(hash_table_delete(fx.ht, fx.lookups[CHAIN_FRONT_IDX]) == HT_OK);
    CHECK(hash_table_count(fx.ht) == COLLIDING_KEY_COUNT - 1);
    CHECK(is_only_missing(&fx, CHAIN_FRONT_IDX));
    collision_fixture_deinit(&fx);
}

static void test_delete_chain_middle_keeps_others(void)
{
    struct collision_fixture fx;
    collision_fixture_init(&fx);
    CHECK(hash_table_delete(fx.ht, fx.lookups[CHAIN_MIDDLE_IDX]) == HT_OK);
    CHECK(hash_table_count(fx.ht) == COLLIDING_KEY_COUNT - 1);
    CHECK(is_only_missing(&fx, CHAIN_MIDDLE_IDX));
    collision_fixture_deinit(&fx);
}

static void test_delete_chain_end_keeps_others(void)
{
    struct collision_fixture fx;
    collision_fixture_init(&fx);
    CHECK(hash_table_delete(fx.ht, fx.lookups[CHAIN_END_IDX]) == HT_OK);
    CHECK(hash_table_count(fx.ht) == COLLIDING_KEY_COUNT - 1);
    CHECK(is_only_missing(&fx, CHAIN_END_IDX));
    collision_fixture_deinit(&fx);
}

static void test_delete_all_colliding_keys_empties_table(void)
{
    struct collision_fixture fx;
    collision_fixture_init(&fx);
    bool is_all_deleted = true;
    for (size_t i = 0; i < COLLIDING_KEY_COUNT; i++) {
        is_all_deleted = is_all_deleted && hash_table_delete(fx.ht, fx.lookups[i]) == HT_OK;
    }
    CHECK(is_all_deleted);
    CHECK(hash_table_count(fx.ht) == 0);
    collision_fixture_deinit(&fx);
}

static void test_delete_on_empty_table_returns_not_found(void)
{
    struct hash_table *ht = hash_table_create(ignore_value);
    struct str *lookup = create_from_text("k");
    CHECK(hash_table_delete(ht, lookup) == HT_NOT_FOUND);
    CHECK(hash_table_count(ht) == 0);
    str_destroy(lookup);
    hash_table_destroy(ht);
}

static void test_delete_missing_key_returns_not_found(void)
{
    static int value = 1;
    struct hash_table *ht = hash_table_create(ignore_value);
    hash_table_set(ht, create_from_text("a"), &value);
    struct str *lookup = create_from_text("b");
    CHECK(hash_table_delete(ht, lookup) == HT_NOT_FOUND);
    CHECK(hash_table_count(ht) == 1);
    str_destroy(lookup);
    hash_table_destroy(ht);
}

static void test_delete_twice_returns_not_found(void)
{
    static int value = 1;
    struct hash_table *ht = hash_table_create(ignore_value);
    hash_table_set(ht, create_from_text("k"), &value);
    struct str *lookup = create_from_text("k");
    CHECK(hash_table_delete(ht, lookup) == HT_OK);
    CHECK(hash_table_delete(ht, lookup) == HT_NOT_FOUND);
    CHECK(hash_table_find(ht, lookup) == NULL);
    str_destroy(lookup);
    hash_table_destroy(ht);
}

static void test_delete_destroys_value_once(void)
{
    static int value = 1;
    g_destroyed_count = 0;
    struct hash_table *ht = hash_table_create(count_destroyed_value);
    hash_table_set(ht, create_from_text("k"), &value);
    struct str *lookup = create_from_text("k");
    hash_table_delete(ht, lookup);
    CHECK(g_destroyed_count == 1);
    hash_table_destroy(ht);
    CHECK(g_destroyed_count == 1);
    str_destroy(lookup);
}

static void test_delete_frees_key_and_value_bytes(void)
{
    struct hash_table *ht = hash_table_create(destroy_str_value);
    struct str *lookup = create_from_text("k");
    size_t before_bytes = mem_used_bytes();
    hash_table_set(ht, create_from_text("k"), create_from_text("value"));
    hash_table_delete(ht, lookup);
    CHECK(mem_used_bytes() == before_bytes);
    str_destroy(lookup);
    hash_table_destroy(ht);
}

static void test_set_after_delete_stores_new_value(void)
{
    static int first = 1;
    static int second = 2;
    struct hash_table *ht = hash_table_create(ignore_value);
    struct str *lookup = create_from_text("k");
    hash_table_set(ht, create_from_text("k"), &first);
    hash_table_delete(ht, lookup);
    CHECK(hash_table_set(ht, create_from_text("k"), &second) == HT_OK);
    CHECK(hash_table_count(ht) == 1);
    CHECK(hash_table_find(ht, lookup)->value == &second);
    str_destroy(lookup);
    hash_table_destroy(ht);
}

static void test_delete_plain_key_keeps_trailing_nul_key(void)
{
    static const char raw[] = {'a', '\0'};
    static int value = 1;
    struct hash_table *ht = hash_table_create(ignore_value);
    hash_table_set(ht, create_from_text("a"), &value);
    hash_table_set(ht, str_create(raw, sizeof(raw)), &value);
    struct str *plain = create_from_text("a");
    struct str *with_nul = str_create(raw, sizeof(raw));
    CHECK(hash_table_delete(ht, plain) == HT_OK);
    CHECK(hash_table_count(ht) == 1);
    CHECK(hash_table_find(ht, with_nul) != NULL);
    str_destroy(plain);
    str_destroy(with_nul);
    hash_table_destroy(ht);
}

static void test_random_entry_on_empty_table_returns_null(void)
{
    struct hash_table *ht = hash_table_create(ignore_value);
    CHECK(hash_table_random_entry(ht) == NULL);
    hash_table_destroy(ht);
}

static void test_random_entry_with_one_entry_always_returns_it(void)
{
    static int value = 1;
    struct hash_table *ht = hash_table_create(ignore_value);
    hash_table_set(ht, create_from_text("only"), &value);
    bool is_always_same = true;
    for (int i = 0; i < RANDOM_TRIAL_COUNT; i++) {
        const struct ht_entry *entry = hash_table_random_entry(ht);
        is_always_same = is_always_same && entry != NULL && entry->value == &value;
    }
    CHECK(is_always_same);
    hash_table_destroy(ht);
}

static void test_random_entry_after_deleting_all_returns_null(void)
{
    static int value = 1;
    struct hash_table *ht = hash_table_create(ignore_value);
    struct str *lookup = create_from_text("k");
    hash_table_set(ht, create_from_text("k"), &value);
    hash_table_delete(ht, lookup);
    CHECK(hash_table_random_entry(ht) == NULL);
    str_destroy(lookup);
    hash_table_destroy(ht);
}

static void test_random_entry_reaches_every_entry_in_chain(void)
{
    struct collision_fixture fx;
    collision_fixture_init(&fx);
    bool is_seen[COLLIDING_KEY_COUNT] = {false};
    for (int i = 0; i < RANDOM_TRIAL_COUNT; i++) {
        const struct ht_entry *entry = hash_table_random_entry(fx.ht);
        is_seen[*(const int *)entry->value] = true;
    }
    bool is_all_seen = true;
    for (size_t i = 0; i < COLLIDING_KEY_COUNT; i++) {
        is_all_seen = is_all_seen && is_seen[i];
    }
    CHECK(is_all_seen);
    collision_fixture_deinit(&fx);
}

static void test_random_entry_reaches_entries_in_different_buckets(void)
{
    static int values[ENTRY_COUNT];
    struct hash_table *ht = hash_table_create(ignore_value);
    for (int i = 0; i < ENTRY_COUNT; i++) {
        char buf[KEY_BUF_BYTES];
        int len = snprintf(buf, sizeof(buf), "key:%d", i);
        values[i] = i;
        hash_table_set(ht, str_create(buf, (size_t)len), &values[i]);
    }
    bool is_seen[ENTRY_COUNT] = {false};
    for (int i = 0; i < RANDOM_TRIAL_COUNT; i++) {
        const struct ht_entry *entry = hash_table_random_entry(ht);
        is_seen[*(const int *)entry->value] = true;
    }
    bool is_all_seen = true;
    for (size_t i = 0; i < ENTRY_COUNT; i++) {
        is_all_seen = is_all_seen && is_seen[i];
    }
    CHECK(is_all_seen);
    hash_table_destroy(ht);
}

static void test_set_beyond_initial_size_doubles_buckets(void)
{
    static int value = 1;
    struct hash_table *ht = hash_table_create(ignore_value);
    set_numbered_keys(ht, HT_INITIAL_SIZE, &value);
    CHECK(hash_table_bucket_count(ht) == HT_INITIAL_SIZE);
    hash_table_set(ht, create_from_text("one-more"), &value);
    CHECK(hash_table_bucket_count(ht) == (size_t)HT_INITIAL_SIZE * 2);
    hash_table_destroy(ht);
}

static void test_grow_keeps_all_keys_findable(void)
{
    static int value = 1;
    struct hash_table *ht = hash_table_create(ignore_value);
    set_numbered_keys(ht, BIG_KEY_COUNT, &value);
    CHECK(hash_table_count(ht) == BIG_KEY_COUNT);
    CHECK(is_all_numbered_found(ht, 0, BIG_KEY_COUNT));
    hash_table_destroy(ht);
}

static void test_grow_keeps_load_factor_at_most_one(void)
{
    static int value = 1;
    struct hash_table *ht = hash_table_create(ignore_value);
    set_numbered_keys(ht, BIG_KEY_COUNT, &value);
    CHECK(hash_table_count(ht) <= hash_table_bucket_count(ht));
    CHECK(is_power_of_two(hash_table_bucket_count(ht)));
    hash_table_destroy(ht);
}

static void test_delete_most_keys_shrinks_buckets(void)
{
    static int value = 1;
    struct hash_table *ht = hash_table_create(ignore_value);
    set_numbered_keys(ht, BIG_KEY_COUNT, &value);
    size_t peak_bucket_count = hash_table_bucket_count(ht);
    delete_numbered_keys(ht, KEPT_KEY_COUNT, BIG_KEY_COUNT);
    size_t bucket_count = hash_table_bucket_count(ht);
    CHECK(bucket_count < peak_bucket_count);
    CHECK(bucket_count >= HT_INITIAL_SIZE);
    CHECK(is_power_of_two(bucket_count));
    CHECK(is_all_numbered_found(ht, 0, KEPT_KEY_COUNT));
    hash_table_destroy(ht);
}

static void test_delete_all_keys_returns_to_initial_size(void)
{
    static int value = 1;
    struct hash_table *ht = hash_table_create(ignore_value);
    set_numbered_keys(ht, BIG_KEY_COUNT, &value);
    delete_numbered_keys(ht, 0, BIG_KEY_COUNT);
    run_rehash_to_end(ht);
    CHECK(hash_table_count(ht) == 0);
    CHECK(hash_table_bucket_count(ht) == HT_INITIAL_SIZE);
    hash_table_destroy(ht);
}

static void test_delete_at_initial_size_does_not_shrink(void)
{
    static int value = 1;
    struct hash_table *ht = hash_table_create(ignore_value);
    set_numbered_keys(ht, 1, &value);
    delete_numbered_keys(ht, 0, 1);
    CHECK(hash_table_bucket_count(ht) == HT_INITIAL_SIZE);
    hash_table_destroy(ht);
}

static void test_grow_and_shrink_does_not_leak(void)
{
    size_t before_bytes = mem_used_bytes();
    struct hash_table *ht = hash_table_create(destroy_str_value);
    for (int i = 0; i < CHURN_KEY_COUNT; i++) {
        hash_table_set(ht, create_numbered_key(i), create_from_text("value"));
    }
    delete_numbered_keys(ht, 0, CHURN_KEY_COUNT);
    hash_table_destroy(ht);
    CHECK(mem_used_bytes() == before_bytes);
}

static void test_create_is_not_rehashing(void)
{
    struct hash_table *ht = hash_table_create(ignore_value);
    CHECK(!hash_table_is_rehashing(ht));
    hash_table_destroy(ht);
}

enum {
    REHASH_STEP_LIMIT = 1000000,
    GROWN_SIZE = HT_INITIAL_SIZE * 2,
    LAST_BUCKET_IDX = GROWN_SIZE - 1,
    LAST_BUCKET_KEY_COUNT = 3,
    FILLER_KEY_COUNT = HT_INITIAL_SIZE + 1 - LAST_BUCKET_KEY_COUNT,
    GROW_KEY_COUNT = HT_INITIAL_SIZE + 1,
    PARTIAL_STEP_COUNT = 4,
    MORE_KEY_COUNT = 40,
    FIND_REPEAT_COUNT = 100,
};

/* rehash 가 끝날 때까지 rehash_step(ht, 1) 을 부르고 부른 횟수를 돌려준다 */
static size_t run_rehash_to_end(struct hash_table *ht)
{
    size_t step_count = 0;
    while (hash_table_is_rehashing(ht) && step_count < REHASH_STEP_LIMIT) {
        hash_table_rehash_step(ht, 1);
        step_count++;
    }
    return step_count;
}

/* key:0 ~ key:16 을 넣어 늘리기를 시작시키고, 엔트리(노드 + 키)가 차지하는 바이트를 돌려준다 */
static size_t set_keys_until_grow(struct hash_table *ht, void *value)
{
    size_t entry_bytes = 0;
    for (int i = 0; i <= HT_INITIAL_SIZE; i++) {
        struct str *key = create_numbered_key(i);
        entry_bytes += sizeof(struct ht_entry) + sizeof(*key) + key->len + 1;
        hash_table_set(ht, key, value);
    }
    return entry_bytes;
}

/* 32칸 테이블의 마지막 버킷(31번)에 떨어지는 키를 count 개 만든다 */
static void create_last_bucket_keys(struct str **keys, size_t count)
{
    size_t found = 0;
    for (int i = 0; found < count && i < COLLISION_SEARCH_LIMIT; i++) {
        char buf[KEY_BUF_BYTES];
        int len = snprintf(buf, sizeof(buf), "last:%d", i);
        struct str *key = str_create(buf, (size_t)len);
        if ((hash_table_hash(key) & LAST_BUCKET_IDX) == LAST_BUCKET_IDX) {
            keys[found++] = key;
        } else {
            str_destroy(key);
        }
    }
    CHECK(found == count);
}

/* 32칸으로 늘린 뒤 채움 키를 지워, 31번 버킷에만 키 3개가 남은 채로 줄이기 rehash 가 막 시작된
 * 테이블 */
static struct hash_table *create_shrinking_table_with_last_bucket_keys(void)
{
    static int value = 1;
    struct str *last_keys[LAST_BUCKET_KEY_COUNT];
    create_last_bucket_keys(last_keys, LAST_BUCKET_KEY_COUNT);
    struct hash_table *ht = hash_table_create(ignore_value);
    for (size_t i = 0; i < LAST_BUCKET_KEY_COUNT; i++) {
        hash_table_set(ht, last_keys[i], &value);
    }
    set_numbered_keys(ht, FILLER_KEY_COUNT, &value);
    delete_numbered_keys(ht, 0, FILLER_KEY_COUNT);
    return ht;
}

static void test_grow_and_shrink_then_rehash_to_end_is_not_rehashing(void)
{
    static int value = 1;
    struct hash_table *ht = hash_table_create(ignore_value);
    set_numbered_keys(ht, CHURN_KEY_COUNT, &value);
    run_rehash_to_end(ht);
    CHECK(!hash_table_is_rehashing(ht));
    delete_numbered_keys(ht, 0, CHURN_KEY_COUNT);
    run_rehash_to_end(ht);
    CHECK(!hash_table_is_rehashing(ht));
    hash_table_destroy(ht);
}

/* 늘리기가 끝나면 32칸 버킷 배열 하나만 남는다: 옛 16칸 배열은 해제되고 tables[1] 은 비어 있다 */
static void test_grow_keeps_only_one_bucket_array(void)
{
    static int value = 1;
    struct hash_table *ht = hash_table_create(ignore_value);
    size_t created_bytes = mem_used_bytes();
    size_t entry_bytes = set_keys_until_grow(ht, &value);
    run_rehash_to_end(ht);
    size_t added_bucket_bytes = (size_t)(GROWN_SIZE - HT_INITIAL_SIZE) * sizeof(struct ht_entry *);
    CHECK(hash_table_bucket_count(ht) == GROWN_SIZE);
    CHECK(!hash_table_is_rehashing(ht));
    CHECK(mem_used_bytes() == created_bytes + entry_bytes + added_bucket_bytes);
    hash_table_destroy(ht);
}

static void test_shrink_keeps_only_one_bucket_array(void)
{
    static int value = 1;
    struct hash_table *ht = hash_table_create(ignore_value);
    size_t created_bytes = mem_used_bytes();
    set_numbered_keys(ht, CHURN_KEY_COUNT, &value);
    delete_numbered_keys(ht, 0, CHURN_KEY_COUNT);
    run_rehash_to_end(ht);
    CHECK(hash_table_bucket_count(ht) == HT_INITIAL_SIZE);
    CHECK(!hash_table_is_rehashing(ht));
    CHECK(mem_used_bytes() == created_bytes);
    hash_table_destroy(ht);
}

/* 초과 직후: rehash 중이고, 옛 16칸과 새 32칸 배열이 함께 있으며, 키는 모두 조회된다 */
static void test_load_factor_exceeded_starts_rehash_with_both_arrays(void)
{
    static int value = 1;
    struct hash_table *ht = hash_table_create(ignore_value);
    size_t created_bytes = mem_used_bytes();
    size_t entry_bytes = set_keys_until_grow(ht, &value);
    size_t new_bucket_bytes = (size_t)GROWN_SIZE * sizeof(struct ht_entry *);
    CHECK(hash_table_is_rehashing(ht));
    CHECK(hash_table_bucket_count(ht) == GROWN_SIZE);
    CHECK(mem_used_bytes() == created_bytes + entry_bytes + new_bucket_bytes);
    CHECK(is_all_numbered_found(ht, 0, HT_INITIAL_SIZE + 1));
    hash_table_destroy(ht);
}

static void test_rehash_step_zero_moves_nothing(void)
{
    static int value = 1;
    struct hash_table *ht = hash_table_create(ignore_value);
    set_keys_until_grow(ht, &value);
    size_t before_bytes = mem_used_bytes();
    hash_table_rehash_step(ht, 0);
    CHECK(hash_table_is_rehashing(ht));
    CHECK(mem_used_bytes() == before_bytes);
    hash_table_destroy(ht);
}

static void test_rehash_step_one_bucket_keeps_rehashing(void)
{
    static int value = 1;
    struct hash_table *ht = hash_table_create(ignore_value);
    set_keys_until_grow(ht, &value);
    hash_table_rehash_step(ht, 1);
    CHECK(hash_table_is_rehashing(ht));
    CHECK(hash_table_count(ht) == HT_INITIAL_SIZE + 1);
    CHECK(is_all_numbered_found(ht, 0, HT_INITIAL_SIZE + 1));
    hash_table_destroy(ht);
}

static void test_rehash_step_all_buckets_finishes_in_one_call(void)
{
    static int value = 1;
    struct hash_table *ht = hash_table_create(ignore_value);
    set_keys_until_grow(ht, &value);
    hash_table_rehash_step(ht, HT_INITIAL_SIZE);
    CHECK(!hash_table_is_rehashing(ht));
    CHECK(hash_table_bucket_count(ht) == GROWN_SIZE);
    CHECK(is_all_numbered_found(ht, 0, HT_INITIAL_SIZE + 1));
    hash_table_destroy(ht);
}

/* 0~30번이 비고 31번에만 키가 있으면 한 번에 빈 버킷 10개씩만 지나쳐 4번 만에 끝난다 */
static void test_rehash_step_limits_empty_bucket_visits(void)
{
    struct hash_table *ht = create_shrinking_table_with_last_bucket_keys();
    CHECK(hash_table_is_rehashing(ht));
    CHECK(hash_table_count(ht) == LAST_BUCKET_KEY_COUNT);
    CHECK(run_rehash_to_end(ht) == LAST_BUCKET_IDX / HT_REHASH_EMPTY_VISITS + 1);
    CHECK(hash_table_bucket_count(ht) == HT_INITIAL_SIZE);
    CHECK(hash_table_count(ht) == LAST_BUCKET_KEY_COUNT);
    hash_table_destroy(ht);
}

static void test_rehash_step_when_not_rehashing_does_nothing(void)
{
    struct hash_table *ht = hash_table_create(ignore_value);
    hash_table_rehash_step(ht, HT_INITIAL_SIZE);
    CHECK(!hash_table_is_rehashing(ht));
    CHECK(hash_table_bucket_count(ht) == HT_INITIAL_SIZE);
    hash_table_destroy(ht);
}

/* key:0 ~ key:16 에 values[i] 를 넣어 늘리기를 시작시킨 뒤 rehash 를 몇 칸만 진행한다 */
static struct hash_table *create_half_rehashed_table(int *values)
{
    struct hash_table *ht = hash_table_create(ignore_value);
    for (int i = 0; i < GROW_KEY_COUNT; i++) {
        values[i] = i;
        hash_table_set(ht, create_numbered_key(i), &values[i]);
    }
    for (int i = 0; i < PARTIAL_STEP_COUNT; i++) {
        hash_table_rehash_step(ht, 1);
    }
    return ht;
}

static void test_find_during_rehash_finds_moved_and_unmoved_keys(void)
{
    int values[GROW_KEY_COUNT];
    struct hash_table *ht = create_half_rehashed_table(values);
    CHECK(hash_table_is_rehashing(ht));
    CHECK(is_all_numbered_found(ht, 0, GROW_KEY_COUNT));
    hash_table_destroy(ht);
}

static void test_find_does_not_advance_rehash(void)
{
    int values[GROW_KEY_COUNT];
    struct hash_table *ht = create_half_rehashed_table(values);
    struct str *lookup = create_numbered_key(0);
    for (int i = 0; i < FIND_REPEAT_COUNT; i++) {
        hash_table_find(ht, lookup);
    }
    CHECK(hash_table_is_rehashing(ht));
    str_destroy(lookup);
    hash_table_destroy(ht);
}

static void test_set_calls_alone_finish_rehash(void)
{
    static int value = 1;
    struct hash_table *ht = hash_table_create(ignore_value);
    set_keys_until_grow(ht, &value);
    for (int i = 0; i < HT_INITIAL_SIZE; i++) {
        hash_table_set(ht, create_numbered_key(0), &value);
    }
    CHECK(!hash_table_is_rehashing(ht));
    CHECK(hash_table_count(ht) == GROW_KEY_COUNT);
    hash_table_destroy(ht);
}

static void test_set_during_rehash_keeps_new_keys_after_rehash_ends(void)
{
    int values[GROW_KEY_COUNT];
    static int value = 1;
    struct hash_table *ht = create_half_rehashed_table(values);
    for (int i = GROW_KEY_COUNT; i < MORE_KEY_COUNT; i++) {
        hash_table_set(ht, create_numbered_key(i), &value);
    }
    run_rehash_to_end(ht);
    CHECK(hash_table_count(ht) == MORE_KEY_COUNT);
    CHECK(is_all_numbered_found(ht, 0, MORE_KEY_COUNT));
    hash_table_destroy(ht);
}

static void test_overwrite_during_rehash_keeps_single_entry(void)
{
    int values[GROW_KEY_COUNT];
    static int new_value = -1;
    struct hash_table *ht = create_half_rehashed_table(values);
    set_numbered_keys(ht, GROW_KEY_COUNT, &new_value);
    CHECK(hash_table_count(ht) == GROW_KEY_COUNT);
    run_rehash_to_end(ht);
    bool is_all_new = true;
    for (int i = 0; i < GROW_KEY_COUNT; i++) {
        struct str *lookup = create_numbered_key(i);
        is_all_new = is_all_new && hash_table_find(ht, lookup)->value == &new_value;
        str_destroy(lookup);
    }
    CHECK(is_all_new);
    CHECK(hash_table_count(ht) == GROW_KEY_COUNT);
    hash_table_destroy(ht);
}

static void test_delete_during_rehash_removes_from_either_table(void)
{
    int values[GROW_KEY_COUNT];
    struct hash_table *ht = create_half_rehashed_table(values);
    bool is_all_deleted = true;
    for (int i = 0; i < GROW_KEY_COUNT; i++) {
        struct str *lookup = create_numbered_key(i);
        is_all_deleted = is_all_deleted && hash_table_delete(ht, lookup) == HT_OK;
        is_all_deleted = is_all_deleted && hash_table_find(ht, lookup) == NULL;
        str_destroy(lookup);
    }
    CHECK(is_all_deleted);
    CHECK(hash_table_count(ht) == 0);
    run_rehash_to_end(ht);
    CHECK(!hash_table_is_rehashing(ht));
    hash_table_destroy(ht);
}

/* 늘리기 rehash 도중 대부분을 지워 줄이기 조건이 와도 새 rehash 를 겹쳐 시작하지 않는다 */
static void test_shrink_condition_during_rehash_is_safe(void)
{
    int values[GROW_KEY_COUNT];
    struct hash_table *ht = create_half_rehashed_table(values);
    delete_numbered_keys(ht, 1, GROW_KEY_COUNT);
    CHECK(hash_table_count(ht) == 1);
    CHECK(is_all_numbered_found(ht, 0, 1));
    run_rehash_to_end(ht);
    run_rehash_to_end(ht);
    CHECK(is_power_of_two(hash_table_bucket_count(ht)));
    CHECK(is_all_numbered_found(ht, 0, 1));
    hash_table_destroy(ht);
}

static void test_random_entry_during_rehash_reaches_both_tables(void)
{
    int values[GROW_KEY_COUNT];
    struct hash_table *ht = create_half_rehashed_table(values);
    CHECK(hash_table_is_rehashing(ht));
    bool is_seen[GROW_KEY_COUNT] = {false};
    for (int i = 0; i < RANDOM_TRIAL_COUNT * 2; i++) {
        const struct ht_entry *entry = hash_table_random_entry(ht);
        is_seen[*(const int *)entry->value] = true;
    }
    bool is_all_seen = true;
    for (size_t i = 0; i < GROW_KEY_COUNT; i++) {
        is_all_seen = is_all_seen && is_seen[i];
    }
    CHECK(is_all_seen);
    hash_table_destroy(ht);
}

static void run_create_tests(void)
{
    RUN(test_create_returns_zero_count);
    RUN(test_create_allocates_initial_bucket_count);
    RUN(test_initial_size_is_power_of_two);
    RUN(test_create_without_value_destroy_succeeds);
    RUN(test_destroy_empty_table_returns_used_bytes_to_baseline);
    RUN(test_destroy_null_does_nothing);
    RUN(test_destroy_with_entries_calls_value_destroy_per_entry);
    RUN(test_destroy_with_entries_returns_used_bytes_to_baseline);
}

static void run_hash_tests(void)
{
    RUN(test_hash_matches_siphash_vectors);
    RUN(test_hash_with_different_seed_returns_different_hash);
    RUN(test_load_random_seed_changes_hash);
    RUN(test_table_created_after_random_seed_finds_keys);
    RUN(test_hash_same_key_returns_same_hash);
    RUN(test_hash_differs_after_nul_returns_different_hash);
    RUN(test_hash_trailing_nul_returns_different_hash);
    RUN(test_hash_sequential_keys_cover_all_initial_buckets);
}

static void run_set_find_tests(void)
{
    RUN(test_set_then_find_returns_value);
    RUN(test_find_on_empty_table_returns_null);
    RUN(test_find_missing_key_returns_null);
    RUN(test_set_new_keys_increments_count);
    RUN(test_set_existing_key_overwrites_value);
    RUN(test_set_existing_key_destroys_old_value);
    RUN(test_set_same_value_pointer_keeps_value);
    RUN(test_overwrite_then_destroy_returns_used_bytes_to_baseline);
    RUN(test_colliding_keys_all_findable);
    RUN(test_empty_key_set_and_find);
    RUN(test_keys_differing_by_trailing_nul_are_separate);
}

static void run_delete_tests(void)
{
    RUN(test_delete_chain_front_keeps_others);
    RUN(test_delete_chain_middle_keeps_others);
    RUN(test_delete_chain_end_keeps_others);
    RUN(test_delete_all_colliding_keys_empties_table);
    RUN(test_delete_on_empty_table_returns_not_found);
    RUN(test_delete_missing_key_returns_not_found);
    RUN(test_delete_twice_returns_not_found);
    RUN(test_delete_destroys_value_once);
    RUN(test_delete_frees_key_and_value_bytes);
    RUN(test_set_after_delete_stores_new_value);
    RUN(test_delete_plain_key_keeps_trailing_nul_key);
}

static void run_random_entry_tests(void)
{
    RUN(test_random_entry_on_empty_table_returns_null);
    RUN(test_random_entry_with_one_entry_always_returns_it);
    RUN(test_random_entry_after_deleting_all_returns_null);
    RUN(test_random_entry_reaches_every_entry_in_chain);
    RUN(test_random_entry_reaches_entries_in_different_buckets);
}

static void run_resize_tests(void)
{
    RUN(test_set_beyond_initial_size_doubles_buckets);
    RUN(test_grow_keeps_all_keys_findable);
    RUN(test_grow_keeps_load_factor_at_most_one);
    RUN(test_delete_most_keys_shrinks_buckets);
    RUN(test_delete_all_keys_returns_to_initial_size);
    RUN(test_delete_at_initial_size_does_not_shrink);
    RUN(test_grow_and_shrink_does_not_leak);
}

static void run_two_table_tests(void)
{
    RUN(test_create_is_not_rehashing);
    RUN(test_grow_and_shrink_then_rehash_to_end_is_not_rehashing);
    RUN(test_grow_keeps_only_one_bucket_array);
    RUN(test_shrink_keeps_only_one_bucket_array);
}

static void run_rehash_step_tests(void)
{
    RUN(test_load_factor_exceeded_starts_rehash_with_both_arrays);
    RUN(test_rehash_step_zero_moves_nothing);
    RUN(test_rehash_step_one_bucket_keeps_rehashing);
    RUN(test_rehash_step_all_buckets_finishes_in_one_call);
    RUN(test_rehash_step_limits_empty_bucket_visits);
    RUN(test_rehash_step_when_not_rehashing_does_nothing);
}

static void run_rehash_operation_tests(void)
{
    RUN(test_find_during_rehash_finds_moved_and_unmoved_keys);
    RUN(test_find_does_not_advance_rehash);
    RUN(test_set_calls_alone_finish_rehash);
    RUN(test_set_during_rehash_keeps_new_keys_after_rehash_ends);
    RUN(test_overwrite_during_rehash_keeps_single_entry);
    RUN(test_delete_during_rehash_removes_from_either_table);
    RUN(test_shrink_condition_during_rehash_is_safe);
    RUN(test_random_entry_during_rehash_reaches_both_tables);
}

int main(void)
{
    run_create_tests();
    run_hash_tests();
    run_set_find_tests();
    run_delete_tests();
    run_random_entry_tests();
    run_resize_tests();
    run_two_table_tests();
    run_rehash_step_tests();
    run_rehash_operation_tests();
    TEST_DONE();
    return 0;
}
