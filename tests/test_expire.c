#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "db.h"
#include "expire.h"
#include "mem.h"
#include "test.h"

enum {
    NOW_MS = 1000,
    PAST_EXPIRE_AT_MS = NOW_MS,         /* NOW_MS 에 이미 만료 */
    FUTURE_EXPIRE_AT_MS = NOW_MS + 500, /* NOW_MS 에는 살아 있음 */
    BIG_BUDGET_MS = 10000,
    KEY_BUF_BYTES = 32,
    FEW_KEY_COUNT = 5,
    MANY_KEY_COUNT = 1000,
    BIG_KEY_COUNT = 10000,
    LOW_RATIO_EXPIRED_COUNT = 4,
    REHASH_KEY_COUNT = 20,
    MAX_CYCLE_CALLS = 100,
};

/* prefix:first ~ prefix:(first+count-1) 를 저장한다. expire_at_ms 가 0 이면 TTL 없이 둔다 */
static void set_numbered_keys(struct db *db, const char *prefix, int count, int64_t expire_at_ms)
{
    for (int i = 0; i < count; i++) {
        char buf[KEY_BUF_BYTES];
        int len = snprintf(buf, sizeof(buf), "%s:%d", prefix, i);
        struct str *key = str_create(buf, (size_t)len);
        CHECK(db_set(db, key, str_create("v", 1)) == DB_OK);
        if (expire_at_ms == 0) {
            continue;
        }
        struct str *lookup = str_create(buf, (size_t)len);
        CHECK(db_set_expire(db, lookup, expire_at_ms) == DB_OK);
        str_destroy(lookup);
    }
}

static bool all_numbered_keys_exist(struct db *db, const char *prefix, int count)
{
    for (int i = 0; i < count; i++) {
        char buf[KEY_BUF_BYTES];
        int len = snprintf(buf, sizeof(buf), "%s:%d", prefix, i);
        struct str *lookup = str_create(buf, (size_t)len);
        const struct str *value = db_get(db, lookup, NOW_MS);
        str_destroy(lookup);
        if (value == NULL) {
            return false;
        }
    }
    return true;
}

/* 서버 주기 작업(12단계)처럼 만료 사이클과 rehash 를 번갈아 돌린다.
 * 대량 삭제로 성겨진 테이블은 rehash 로 줄어들어야 다시 잘 뽑힌다 */
static void run_cycles_until_no_ttl_keys(struct db *db)
{
    for (int i = 0; i < MAX_CYCLE_CALLS && db_ttl_count(db) > 0; i++) {
        expire_cycle(db, NOW_MS, BIG_BUDGET_MS);
        db_rehash_for_ms(db, BIG_BUDGET_MS);
    }
}

static void test_sample_empty_expires_returns_zero(void)
{
    struct db *db = db_create();
    set_numbered_keys(db, "plain", FEW_KEY_COUNT, 0);
    CHECK(expire_sample(db, NOW_MS) == 0);
    CHECK(db_count(db) == FEW_KEY_COUNT);
    db_destroy(db);
}

static void test_sample_fewer_than_sample_count_deletes_all_expired(void)
{
    struct db *db = db_create();
    set_numbered_keys(db, "ttl", FEW_KEY_COUNT, PAST_EXPIRE_AT_MS);
    CHECK(expire_sample(db, NOW_MS) == FEW_KEY_COUNT);
    CHECK(db_count(db) == 0);
    CHECK(db_ttl_count(db) == 0);
    db_destroy(db);
}

static void test_sample_live_ttl_keys_deletes_nothing(void)
{
    struct db *db = db_create();
    set_numbered_keys(db, "ttl", FEW_KEY_COUNT, FUTURE_EXPIRE_AT_MS);
    CHECK(expire_sample(db, NOW_MS) == 0);
    CHECK(db_ttl_count(db) == FEW_KEY_COUNT);
    db_destroy(db);
}

static void test_sample_deletes_at_most_sample_count(void)
{
    struct db *db = db_create();
    set_numbered_keys(db, "ttl", MANY_KEY_COUNT, PAST_EXPIRE_AT_MS);
    CHECK(expire_sample(db, NOW_MS) == EXPIRE_SAMPLE_COUNT);
    CHECK(db_ttl_count(db) == MANY_KEY_COUNT - EXPIRE_SAMPLE_COUNT);
    db_destroy(db);
}

static void test_cycle_many_expired_keys_deletes_only_expired(void)
{
    struct db *db = db_create();
    set_numbered_keys(db, "plain", BIG_KEY_COUNT, 0);
    set_numbered_keys(db, "ttl", BIG_KEY_COUNT, PAST_EXPIRE_AT_MS);

    struct expire_stats stats = expire_cycle(db, NOW_MS, BIG_BUDGET_MS);
    CHECK(stats.sample_rounds > 1);
    run_cycles_until_no_ttl_keys(db);

    CHECK(db_ttl_count(db) == 0);
    CHECK(db_count(db) == BIG_KEY_COUNT);
    CHECK(all_numbered_keys_exist(db, "plain", BIG_KEY_COUNT));
    db_destroy(db);
}

static void test_cycle_never_deletes_keys_without_ttl(void)
{
    struct db *db = db_create();
    set_numbered_keys(db, "plain", MANY_KEY_COUNT, 0);
    struct expire_stats stats = expire_cycle(db, INT64_MAX, BIG_BUDGET_MS);
    CHECK(stats.sample_rounds == 1);
    CHECK(stats.deleted_count == 0);
    CHECK(db_count(db) == MANY_KEY_COUNT);
    db_destroy(db);
}

static void test_cycle_zero_budget_samples_once(void)
{
    struct db *db = db_create();
    set_numbered_keys(db, "ttl", MANY_KEY_COUNT, PAST_EXPIRE_AT_MS);
    struct expire_stats stats = expire_cycle(db, NOW_MS, 0);
    CHECK(stats.sample_rounds == 1);
    CHECK(stats.deleted_count == EXPIRE_SAMPLE_COUNT);
    db_destroy(db);
}

/* 만료 키가 4개뿐이라 한 표본(20개)에서 많아야 4개(20%)만 지워져 기준 25% 에 못 미친다 */
static void test_cycle_low_expired_ratio_stops_after_one_round(void)
{
    struct db *db = db_create();
    set_numbered_keys(db, "live", MANY_KEY_COUNT, FUTURE_EXPIRE_AT_MS);
    set_numbered_keys(db, "dead", LOW_RATIO_EXPIRED_COUNT, PAST_EXPIRE_AT_MS);
    struct expire_stats stats = expire_cycle(db, NOW_MS, BIG_BUDGET_MS);
    CHECK(stats.sample_rounds == 1);
    CHECK(stats.deleted_count <= LOW_RATIO_EXPIRED_COUNT);
    db_destroy(db);
}

/* TTL 키 17번째에서 expires 가 늘어나기 시작하고, 이후 키는 두 번째 테이블에 들어간다 */
static void test_cycle_during_rehash_cleans_both_tables(void)
{
    struct db *db = db_create();
    set_numbered_keys(db, "ttl", REHASH_KEY_COUNT, PAST_EXPIRE_AT_MS);
    run_cycles_until_no_ttl_keys(db);
    CHECK(db_ttl_count(db) == 0);
    CHECK(db_count(db) == 0);
    db_destroy(db);
}

static void test_cycle_returns_used_bytes_to_baseline(void)
{
    size_t baseline_bytes = mem_used_bytes();
    struct db *db = db_create();
    set_numbered_keys(db, "ttl", MANY_KEY_COUNT, PAST_EXPIRE_AT_MS);
    run_cycles_until_no_ttl_keys(db);
    db_destroy(db);
    CHECK(mem_used_bytes() == baseline_bytes);
}

static void run_sample_tests(void)
{
    RUN(test_sample_empty_expires_returns_zero);
    RUN(test_sample_fewer_than_sample_count_deletes_all_expired);
    RUN(test_sample_live_ttl_keys_deletes_nothing);
    RUN(test_sample_deletes_at_most_sample_count);
}

static void run_cycle_tests(void)
{
    RUN(test_cycle_many_expired_keys_deletes_only_expired);
    RUN(test_cycle_never_deletes_keys_without_ttl);
    RUN(test_cycle_zero_budget_samples_once);
    RUN(test_cycle_low_expired_ratio_stops_after_one_round);
    RUN(test_cycle_during_rehash_cleans_both_tables);
    RUN(test_cycle_returns_used_bytes_to_baseline);
}

int main(void)
{
    run_sample_tests();
    run_cycle_tests();
    TEST_DONE();
    return 0;
}
