#include "test.h"

#include "db.h"
#include "expire.h"

/* ---- 가짜 시계 ---- */

static int64_t fake_now = 1000000;
static int64_t fake_clock(void)
{
    return fake_now;
}

static db *fresh(void)
{
    expire_set_clock(fake_clock);
    fake_now = 1000000;
    db *d = db_create(0, DB_NOEVICTION);
    CHECK(d != NULL);
    return d;
}

static int set_s(db *d, const char *k, const char *v)
{
    return db_set(d, k, strlen(k), v, strlen(v));
}
static entry *get_s(db *d, const char *k)
{
    return db_lookup(d, k, strlen(k));
}
static int val_is(const entry *e, const char *v)
{
    return e != NULL && e->vallen == strlen(v) && memcmp(e->val, v, e->vallen) == 0;
}
#define K(s) (s), strlen(s)

/* TC-DB-001 EP: 저장·조회·삭제 */
static void test_basic(void)
{
    db *d = fresh();
    CHECK(set_s(d, "a", "1") == 0);
    CHECK(val_is(get_s(d, "a"), "1"));
    CHECK(get_s(d, "b") == NULL);
    CHECK(db_size(d) == 1);
    CHECK(db_del(d, K("a")) == 1);
    CHECK(db_del(d, K("a")) == 0);
    CHECK(get_s(d, "a") == NULL);
    db_free(d);
}

/* TC-DB-002 BVA: 만료 경계. now == when 은 아직 살아 있고, when + 1 에서 만료 */
static void test_expire_boundary(void)
{
    db *d = fresh();
    set_s(d, "k", "v");
    CHECK(db_set_expire(d, K("k"), fake_now + 100) == 1);
    fake_now += 99;
    CHECK(val_is(get_s(d, "k"), "v"));
    CHECK(db_ttl_ms(d, K("k")) == 1);
    fake_now += 1; /* now == when */
    CHECK(val_is(get_s(d, "k"), "v"));
    CHECK(db_ttl_ms(d, K("k")) == 0);
    fake_now += 1;
    CHECK(get_s(d, "k") == NULL);
    CHECK(db_ttl_ms(d, K("k")) == -2);
    db_free(d);
}

/* TC-DB-003 ST: lazy 는 실제로 두 테이블에서 지운다 */
static void test_lazy_removes_both(void)
{
    db *d = fresh();
    set_s(d, "k", "v");
    db_set_expire(d, K("k"), fake_now + 10);
    CHECK(dict_len(d->main) == 1 && dict_len(d->expires) == 1);
    fake_now += 11;
    CHECK(dict_len(d->main) == 1); /* 아무도 안 봤으니 아직 남아 있음 */
    CHECK(get_s(d, "k") == NULL);
    CHECK(dict_len(d->main) == 0 && dict_len(d->expires) == 0);
    CHECK(db_used_memory(d) == 0);
    db_free(d);
}

/* TC-DB-004 ST: SET 덮어쓰기는 TTL 을 지운다 */
static void test_set_clears_ttl(void)
{
    db *d = fresh();
    set_s(d, "k", "v1");
    db_set_expire(d, K("k"), fake_now + 10);
    set_s(d, "k", "v2");
    CHECK(db_ttl_ms(d, K("k")) == -1);
    fake_now += 1000;
    CHECK(val_is(get_s(d, "k"), "v2"));
    db_free(d);
}

/* TC-DB-005 EG: 없는 키, 만료된 키에는 만료를 걸 수 없다 */
static void test_expire_missing(void)
{
    db *d = fresh();
    CHECK(db_set_expire(d, K("nope"), fake_now + 10) == 0);
    set_s(d, "k", "v");
    db_set_expire(d, K("k"), fake_now + 5);
    fake_now += 6;
    CHECK(db_set_expire(d, K("k"), fake_now + 100) == 0);
    CHECK(get_s(d, "k") == NULL);
    db_free(d);
}

/* TC-DB-006 BVA: 지금 또는 과거 시각으로 만료를 걸면 즉시 삭제하고 1 */
static void test_expire_now_or_past(void)
{
    db *d = fresh();
    set_s(d, "a", "1");
    set_s(d, "b", "1");
    CHECK(db_set_expire(d, K("a"), fake_now) == 1);
    CHECK(db_set_expire(d, K("b"), fake_now - 1000) == 1);
    CHECK(dict_len(d->main) == 0 && dict_len(d->expires) == 0);
    db_free(d);
}

/* TC-DB-007 EP: TTL 값 — 없음 -2, TTL 없음 -1, 남은 ms */
static void test_ttl_values(void)
{
    db *d = fresh();
    CHECK(db_ttl_ms(d, K("x")) == -2);
    set_s(d, "x", "1");
    CHECK(db_ttl_ms(d, K("x")) == -1);
    db_set_expire(d, K("x"), fake_now + 1500);
    CHECK(db_ttl_ms(d, K("x")) == 1500);
    fake_now += 400;
    CHECK(db_ttl_ms(d, K("x")) == 1100);
    db_free(d);
}

/* TC-DB-008 ST: PERSIST 는 TTL 을 없애고, 두 번째는 0 */
static void test_persist(void)
{
    db *d = fresh();
    set_s(d, "k", "v");
    CHECK(db_persist(d, K("k")) == 0); /* TTL 없음 */
    db_set_expire(d, K("k"), fake_now + 10);
    CHECK(db_persist(d, K("k")) == 1);
    CHECK(db_persist(d, K("k")) == 0);
    fake_now += 1000;
    CHECK(val_is(get_s(d, "k"), "v"));
    CHECK(db_persist(d, K("nope")) == 0);
    db_free(d);
}

/* TC-DB-009 ST: DEL 은 TTL 도 지운다 — 다시 만든 키에 옛 TTL 이 남으면 안 된다 */
static void test_del_clears_ttl(void)
{
    db *d = fresh();
    set_s(d, "k", "v");
    db_set_expire(d, K("k"), fake_now + 10);
    db_del(d, K("k"));
    CHECK(dict_len(d->expires) == 0);
    set_s(d, "k", "v2");
    fake_now += 100;
    CHECK(val_is(get_s(d, "k"), "v2"));
    db_free(d);
}

/* TC-DB-010 EG: 만료됐지만 남아 있는 키를 DEL 하면 0 (Redis 와 같음) */
static void test_del_expired_returns_zero(void)
{
    db *d = fresh();
    set_s(d, "k", "v");
    db_set_expire(d, K("k"), fake_now + 10);
    fake_now += 11;
    CHECK(db_del(d, K("k")) == 0);
    CHECK(dict_len(d->main) == 0 && dict_len(d->expires) == 0);
    db_free(d);
}

/* TC-DB-011 EG: key_expired 는 판정만 하고 지우지 않으며, main 의 rehash 를 진행시키지 않는다 (KEYS 순회용) */
static void test_key_expired_no_side_effect(void)
{
    db *d = fresh();
    char k[16];
    for (int i = 0; i < 5; i++) { /* main 을 rehash 도중 상태로 */
        snprintf(k, sizeof k, "k%d", i);
        set_s(d, k, "1");
    }
    CHECK(dict_is_rehashing(d->main));
    set_s(d, "dead", "1");
    db_set_expire(d, K("dead"), fake_now + 10);
    db_set_expire(d, K("k0"), fake_now + 100000);
    fake_now += 11;
    long idx = d->main->rehashidx;
    CHECK(db_key_expired(d, K("k1")) == 0);   /* TTL 없음 */
    CHECK(db_key_expired(d, K("k0")) == 0);   /* 미래 TTL */
    CHECK(db_key_expired(d, K("dead")) == 1); /* 지남 */
    CHECK(db_key_expired(d, K("none")) == 0);
    CHECK(d->main->rehashidx == idx);         /* main 을 건드리지 않음 */
    CHECK(db_size(d) == 6);                   /* 지우지 않음 */
    db_free(d);
}

/* TC-DB-012 ST: FLUSH */
static void test_flush(void)
{
    db *d = fresh();
    set_s(d, "a", "1");
    set_s(d, "b", "2");
    db_set_expire(d, K("a"), fake_now + 10);
    db_flush(d);
    CHECK(db_size(d) == 0 && dict_len(d->expires) == 0 && db_used_memory(d) == 0);
    set_s(d, "a", "3");
    CHECK(val_is(get_s(d, "a"), "3"));
    db_free(d);
}

/* TC-DB-013 EP: 메모리를 정확히 센다 */
static void test_memory_accounting(void)
{
    db *d = fresh();
    size_t main1 = db_entry_cost(1, 3), exp1 = db_entry_cost(1, 8);
    set_s(d, "k", "abc");
    CHECK(db_used_memory(d) == main1);
    set_s(d, "k", "abcdefghij"); /* 값 3 → 10 */
    CHECK(db_used_memory(d) == main1 + 7);
    db_set_expire(d, K("k"), fake_now + 10);
    CHECK(db_used_memory(d) == main1 + 7 + exp1);
    db_set_expire(d, K("k"), fake_now + 20); /* 갱신은 그대로 */
    CHECK(db_used_memory(d) == main1 + 7 + exp1);
    db_persist(d, K("k"));
    CHECK(db_used_memory(d) == main1 + 7);
    db_del(d, K("k"));
    CHECK(db_used_memory(d) == 0);
    db_free(d);
}

/* TC-DB-014 BVA: noeviction — 들어갈 자리가 없으면 -1, 아무것도 안 지움 */
static void test_noeviction(void)
{
    expire_set_clock(fake_clock);
    size_t one = db_entry_cost(1, 1);
    db *d = db_create(one * 2, DB_NOEVICTION);
    CHECK(db_make_room(d, one) == 0);
    set_s(d, "a", "1");
    CHECK(db_make_room(d, one) == 0); /* 정확히 상한까지는 됨 */
    set_s(d, "b", "1");
    CHECK(db_make_room(d, one) == -1);
    CHECK(db_size(d) == 2);
    db_free(d);
}

/* TC-DB-015 ST: allkeys-random — 내보내서 맞추고, TTL 기록도 같이 지운다 */
static void test_allkeys_random(void)
{
    expire_set_clock(fake_clock);
    dict_set_rand(NULL);
    srand(3);
    size_t one = db_entry_cost(2, 1);
    db *d = db_create(one * 10, DB_ALLKEYS_RANDOM);
    char k[3] = "k0";
    for (int i = 0; i < 10; i++) {
        k[1] = (char)('0' + i);
        CHECK(db_make_room(d, one) == 0);
        set_s(d, k, "v");
        db_set_expire(d, k, 2, fake_now + 100000); /* TTL 기록이 메모리를 더 쓰게 */
    }
    CHECK(db_used_memory(d) > d->maxmemory || db_size(d) < 10); /* 이미 넘쳤거나 내보냈음 */
    CHECK(db_make_room(d, one) == 0);
    CHECK(db_used_memory(d) + one <= d->maxmemory);
    CHECK(d->evicted > 0);
    CHECK(dict_len(d->expires) == db_size(d)); /* 내보낸 키의 TTL 이 남지 않음 */
    db_free(d);
}

/* TC-DB-016 BVA: 요청 하나가 상한보다 크면 아무것도 지우지 않고 -1. 상한 0 은 무제한 */
static void test_need_larger_than_max(void)
{
    expire_set_clock(fake_clock);
    db *d = db_create(1000, DB_ALLKEYS_RANDOM);
    set_s(d, "a", "1");
    CHECK(db_make_room(d, 1001) == -1);
    CHECK(db_size(d) == 1 && d->evicted == 0);
    db_free(d);

    d = db_create(0, DB_NOEVICTION);
    CHECK(db_make_room(d, (size_t)1 << 40) == 0);
    db_free(d);
}

/* TC-DB-017 EG: 바이너리 키 */
static void test_binary_key(void)
{
    db *d = fresh();
    CHECK(db_set(d, "a\0b", 3, "1", 1) == 0);
    CHECK(db_set_expire(d, "a\0b", 3, fake_now + 10) == 1);
    CHECK(db_lookup(d, "a", 1) == NULL);
    CHECK(db_ttl_ms(d, "a\0b", 3) == 10);
    db_free(d);
}

int main(void)
{
    RUN(test_basic);
    RUN(test_expire_boundary);
    RUN(test_lazy_removes_both);
    RUN(test_set_clears_ttl);
    RUN(test_expire_missing);
    RUN(test_expire_now_or_past);
    RUN(test_ttl_values);
    RUN(test_persist);
    RUN(test_del_clears_ttl);
    RUN(test_del_expired_returns_zero);
    RUN(test_key_expired_no_side_effect);
    RUN(test_flush);
    RUN(test_memory_accounting);
    RUN(test_noeviction);
    RUN(test_allkeys_random);
    RUN(test_need_larger_than_max);
    RUN(test_binary_key);
    TEST_DONE();
    return 0;
}
