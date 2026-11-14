#include "test.h"

#include "db.h"
#include "expire.h"

static int64_t fake_now = 5000000;
static int64_t fake_clock(void)
{
    return fake_now;
}

/* 키 n개를 만들고, 앞의 expired 개는 이미 지난 TTL, 그다음 future 개는 미래 TTL, 나머지는 TTL 없음 */
static db *make(int n, int expired, int future)
{
    expire_set_clock(fake_clock);
    dict_set_rand(NULL);
    srand(11);
    db *d = db_create(0, DB_NOEVICTION);
    char k[32];
    for (int i = 0; i < n; i++) {
        snprintf(k, sizeof k, "key:%d", i);
        db_set(d, k, strlen(k), "v", 1);
        if (i < expired)
            db_set_expire(d, k, strlen(k), fake_now + 10);
        else if (i < expired + future)
            db_set_expire(d, k, strlen(k), fake_now + 1000000);
    }
    fake_now += 11; /* 앞의 expired 개가 만료됨. 아무도 조회하지 않음 */
    return d;
}

/* TC-EXP-001 EP: 아무도 조회하지 않은 만료 키를 active 가 지운다. TTL 없는 키·미래 키는 그대로 */
static void test_active_removes_unaccessed(void)
{
    db *d = make(300, 100, 100);
    int total = 0, n;
    while ((n = expire_active_cycle(d)) > 0)
        total += n;
    /* 마지막 몇 개는 표본에 잘 안 걸려 남을 수 있다(25% 이하면 멈춤) → 여러 번 돌려 다 지운다 */
    for (int i = 0; i < 200; i++)
        total += expire_active_cycle(d);
    CHECK(total == 100);
    CHECK(db_size(d) == 200);
    CHECK(dict_len(d->expires) == 100);
    db_free(d);
}

/* TC-EXP-002 EP: 지난 키가 없으면 아무것도 안 지운다 */
static void test_active_nothing_expired(void)
{
    db *d = make(100, 0, 50);
    CHECK(expire_active_cycle(d) == 0);
    CHECK(db_size(d) == 100 && dict_len(d->expires) == 50);
    db_free(d);
}

/* TC-EXP-003 ST: 표본의 25% 넘게 지났으면 반복한다 → 전부 지난 1000개는 한 번 호출로 다 지운다 */
static void test_active_repeats(void)
{
    db *d = make(1000, 1000, 0);
    CHECK(expire_active_cycle(d) == 1000);
    CHECK(db_size(d) == 0 && dict_len(d->expires) == 0);
    db_free(d);
}

/* TC-EXP-004 ST: 지난 키가 드물면(1%) 한 번 표본만 보고 멈춘다 */
static void test_active_stops_when_sparse(void)
{
    db *d = make(1000, 10, 990);
    int n = expire_active_cycle(d);
    CHECK(n < 10);
    CHECK(dict_len(d->expires) > 990);
    db_free(d);
}

/* TC-EXP-005 BVA: 빈 db, TTL 키가 하나도 없는 db */
static void test_active_empty(void)
{
    db *d = make(0, 0, 0);
    CHECK(expire_active_cycle(d) == 0);
    db_free(d);
    d = make(10, 0, 0);
    CHECK(expire_active_cycle(d) == 0);
    db_free(d);
}

/* TC-EXP-006 EP: 시계 교체와 복구 */
static void test_clock(void)
{
    expire_set_clock(fake_clock);
    fake_now = 42;
    CHECK(now_ms() == 42);
    expire_set_clock(NULL);
    CHECK(now_ms() > 1700000000000LL); /* 2023년 이후의 실제 시각 */
}

int main(void)
{
    RUN(test_active_removes_unaccessed);
    RUN(test_active_nothing_expired);
    RUN(test_active_repeats);
    RUN(test_active_stops_when_sparse);
    RUN(test_active_empty);
    RUN(test_clock);
    TEST_DONE();
    return 0;
}
