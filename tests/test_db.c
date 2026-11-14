#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "db.h"
#include "mem.h"
#include "test.h"

enum {
    NOW_MS = 1000,
    TTL_MS = 500,
    EXPIRE_AT_MS = NOW_MS + TTL_MS,
    ENTRY_COUNT = 100,
    KEY_BUF_BYTES = 32,
};

static struct str *create_from_text(const char *text)
{
    return str_create(text, strlen(text));
}

static bool value_equals_text(const struct str *value, const char *text)
{
    return value != NULL && value->len == strlen(text) &&
           memcmp(value->data, text, value->len) == 0;
}

static void set_text(struct db *db, const char *key, const char *value)
{
    CHECK(db_set(db, create_from_text(key), create_from_text(value)) == DB_OK);
}

/* key 를 value 로 저장하고 EXPIRE_AT_MS 에 만료되게 한다 */
static void set_text_with_ttl(struct db *db, const char *key, const char *value)
{
    set_text(db, key, value);
    struct str *lookup = create_from_text(key);
    CHECK(db_set_expire(db, lookup, EXPIRE_AT_MS) == DB_OK);
    str_destroy(lookup);
}

static const struct str *get_text(struct db *db, const char *key, int64_t now_ms)
{
    struct str *lookup = create_from_text(key);
    const struct str *value = db_get(db, lookup, now_ms);
    str_destroy(lookup);
    return value;
}

static enum db_result delete_text(struct db *db, const char *key, int64_t now_ms)
{
    struct str *lookup = create_from_text(key);
    enum db_result result = db_delete(db, lookup, now_ms);
    str_destroy(lookup);
    return result;
}

static void test_create_is_empty(void)
{
    struct db *db = db_create();
    CHECK(db != NULL);
    CHECK(db_count(db) == 0);
    db_destroy(db);
}

static void test_destroy_null_does_nothing(void)
{
    db_destroy(NULL);
    CHECK(true);
}

static void test_get_after_set_returns_same_value(void)
{
    struct db *db = db_create();
    set_text(db, "name", "redis");
    CHECK(value_equals_text(get_text(db, "name", NOW_MS), "redis"));
    CHECK(db_count(db) == 1);
    db_destroy(db);
}

static void test_get_missing_key_returns_null(void)
{
    struct db *db = db_create();
    set_text(db, "name", "redis");
    CHECK(get_text(db, "other", NOW_MS) == NULL);
    db_destroy(db);
}

static void test_set_existing_key_overwrites_value(void)
{
    struct db *db = db_create();
    set_text(db, "name", "redis");
    set_text(db, "name", "mini");
    CHECK(value_equals_text(get_text(db, "name", NOW_MS), "mini"));
    CHECK(db_count(db) == 1);
    db_destroy(db);
}

static void test_get_one_ms_before_expire_returns_value(void)
{
    struct db *db = db_create();
    set_text_with_ttl(db, "session", "abc");
    CHECK(value_equals_text(get_text(db, "session", EXPIRE_AT_MS - 1), "abc"));
    CHECK(db_count(db) == 1);
    db_destroy(db);
}

static void test_get_at_expire_time_returns_null(void)
{
    struct db *db = db_create();
    set_text_with_ttl(db, "session", "abc");
    CHECK(get_text(db, "session", EXPIRE_AT_MS) == NULL);
    db_destroy(db);
}

/* 만료된 키는 keys 에서 빠지고(count 0), expires 에서도 빠진다(TTL 을 다시 걸 수 없다) */
static void test_get_after_expire_deletes_from_both_tables(void)
{
    struct db *db = db_create();
    set_text_with_ttl(db, "session", "abc");
    CHECK(get_text(db, "session", EXPIRE_AT_MS + 1) == NULL);
    CHECK(db_count(db) == 0);

    struct str *lookup = create_from_text("session");
    CHECK(db_set_expire(db, lookup, EXPIRE_AT_MS) == DB_NOT_FOUND);
    str_destroy(lookup);
    db_destroy(db);
}

static void test_set_without_ttl_removes_previous_ttl(void)
{
    struct db *db = db_create();
    size_t baseline_bytes = mem_used_bytes();
    set_text_with_ttl(db, "session", "abc");
    set_text(db, "session", "abc");
    CHECK(value_equals_text(get_text(db, "session", EXPIRE_AT_MS + 1), "abc"));
    CHECK(delete_text(db, "session", NOW_MS) == DB_OK);
    CHECK(mem_used_bytes() == baseline_bytes); /* expires 에 남은 엔트리가 없다 */
    db_destroy(db);
}

static void test_set_expire_twice_keeps_latest_time(void)
{
    struct db *db = db_create();
    set_text_with_ttl(db, "session", "abc");
    struct str *lookup = create_from_text("session");
    CHECK(db_set_expire(db, lookup, EXPIRE_AT_MS + TTL_MS) == DB_OK);
    str_destroy(lookup);
    CHECK(value_equals_text(get_text(db, "session", EXPIRE_AT_MS), "abc"));
    CHECK(get_text(db, "session", EXPIRE_AT_MS + TTL_MS) == NULL);
    db_destroy(db);
}

static void test_set_expire_missing_key_returns_not_found(void)
{
    struct db *db = db_create();
    struct str *lookup = create_from_text("missing");
    CHECK(db_set_expire(db, lookup, EXPIRE_AT_MS) == DB_NOT_FOUND);
    str_destroy(lookup);
    CHECK(db_count(db) == 0);
    db_destroy(db);
}

static void test_delete_expired_key_returns_not_found(void)
{
    struct db *db = db_create();
    set_text_with_ttl(db, "session", "abc");
    CHECK(delete_text(db, "session", EXPIRE_AT_MS) == DB_NOT_FOUND);
    CHECK(db_count(db) == 0);
    db_destroy(db);
}

static void test_delete_existing_key_decreases_count(void)
{
    struct db *db = db_create();
    set_text(db, "a", "1");
    set_text_with_ttl(db, "b", "2");
    CHECK(delete_text(db, "a", NOW_MS) == DB_OK);
    CHECK(delete_text(db, "b", NOW_MS) == DB_OK);
    CHECK(db_count(db) == 0);
    CHECK(get_text(db, "b", NOW_MS) == NULL);
    db_destroy(db);
}

static void test_delete_missing_key_returns_not_found(void)
{
    struct db *db = db_create();
    CHECK(delete_text(db, "missing", NOW_MS) == DB_NOT_FOUND);
    db_destroy(db);
}

static void test_destroy_with_entries_returns_used_bytes_to_baseline(void)
{
    size_t baseline_bytes = mem_used_bytes();
    struct db *db = db_create();
    for (int i = 0; i < ENTRY_COUNT; i++) {
        char buf[KEY_BUF_BYTES];
        snprintf(buf, sizeof(buf), "key:%d", i);
        if (i % 2 == 0) {
            set_text_with_ttl(db, buf, "value");
        } else {
            set_text(db, buf, "value");
        }
    }
    CHECK(db_count(db) == ENTRY_COUNT);
    db_destroy(db);
    CHECK(mem_used_bytes() == baseline_bytes);
}

static enum db_result delete_text_if_expired(struct db *db, const char *key, int64_t now_ms)
{
    struct str *lookup = create_from_text(key);
    enum db_result result = db_delete_if_expired(db, lookup, now_ms);
    str_destroy(lookup);
    return result;
}

static void test_delete_if_expired_on_expired_key_deletes_it(void)
{
    struct db *db = db_create();
    set_text_with_ttl(db, "session", "abc");
    CHECK(delete_text_if_expired(db, "session", EXPIRE_AT_MS) == DB_OK);
    CHECK(db_count(db) == 0);
    CHECK(db_ttl_count(db) == 0);
    db_destroy(db);
}

static void test_delete_if_expired_before_expire_keeps_key(void)
{
    struct db *db = db_create();
    set_text_with_ttl(db, "session", "abc");
    CHECK(delete_text_if_expired(db, "session", EXPIRE_AT_MS - 1) == DB_NOT_FOUND);
    CHECK(value_equals_text(get_text(db, "session", EXPIRE_AT_MS - 1), "abc"));
    db_destroy(db);
}

static void test_delete_if_expired_without_ttl_keeps_key(void)
{
    struct db *db = db_create();
    set_text(db, "name", "redis");
    CHECK(delete_text_if_expired(db, "name", EXPIRE_AT_MS) == DB_NOT_FOUND);
    CHECK(delete_text_if_expired(db, "missing", EXPIRE_AT_MS) == DB_NOT_FOUND);
    CHECK(db_count(db) == 1);
    db_destroy(db);
}

static void test_random_ttl_key_without_ttl_keys_returns_null(void)
{
    struct db *db = db_create();
    CHECK(db_random_ttl_key(db) == NULL);
    set_text(db, "name", "redis");
    CHECK(db_random_ttl_key(db) == NULL);
    db_destroy(db);
}

static void test_random_ttl_key_returns_only_ttl_key(void)
{
    struct db *db = db_create();
    set_text(db, "name", "redis");
    set_text_with_ttl(db, "session", "abc");
    const struct str *key = db_random_ttl_key(db);
    CHECK(key != NULL);
    CHECK(value_equals_text(key, "session"));
    CHECK(db_ttl_count(db) == 1);
    db_destroy(db);
}

/* expires 가 소유한 키를 그대로 넘겨도 해제된 키를 읽지 않아야 한다 (valgrind 로 확인) */
static void test_delete_if_expired_with_random_ttl_key_deletes_both(void)
{
    struct db *db = db_create();
    set_text_with_ttl(db, "session", "abc");
    const struct str *key = db_random_ttl_key(db);
    CHECK(db_delete_if_expired(db, key, EXPIRE_AT_MS) == DB_OK);
    CHECK(db_count(db) == 0);
    CHECK(db_ttl_count(db) == 0);
    db_destroy(db);
}

static void run_create_tests(void)
{
    RUN(test_create_is_empty);
    RUN(test_destroy_null_does_nothing);
}

static void run_set_get_tests(void)
{
    RUN(test_get_after_set_returns_same_value);
    RUN(test_get_missing_key_returns_null);
    RUN(test_set_existing_key_overwrites_value);
    RUN(test_set_without_ttl_removes_previous_ttl);
}

static void run_expire_tests(void)
{
    RUN(test_get_one_ms_before_expire_returns_value);
    RUN(test_get_at_expire_time_returns_null);
    RUN(test_get_after_expire_deletes_from_both_tables);
    RUN(test_set_expire_twice_keeps_latest_time);
    RUN(test_set_expire_missing_key_returns_not_found);
}

static void run_delete_tests(void)
{
    RUN(test_delete_expired_key_returns_not_found);
    RUN(test_delete_existing_key_decreases_count);
    RUN(test_delete_missing_key_returns_not_found);
    RUN(test_destroy_with_entries_returns_used_bytes_to_baseline);
}

static void run_active_expire_support_tests(void)
{
    RUN(test_delete_if_expired_on_expired_key_deletes_it);
    RUN(test_delete_if_expired_before_expire_keeps_key);
    RUN(test_delete_if_expired_without_ttl_keeps_key);
    RUN(test_random_ttl_key_without_ttl_keys_returns_null);
    RUN(test_random_ttl_key_returns_only_ttl_key);
    RUN(test_delete_if_expired_with_random_ttl_key_deletes_both);
}

int main(void)
{
    run_create_tests();
    run_set_get_tests();
    run_expire_tests();
    run_delete_tests();
    run_active_expire_support_tests();
    TEST_DONE();
    return 0;
}
