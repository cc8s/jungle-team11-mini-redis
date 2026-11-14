#include "test.h"

#include "hashtable.h"

/* ---- 도우미 ---- */

static int set_s(dict *d, const char *k, const char *v)
{
    return dict_set(d, k, strlen(k), v, strlen(v));
}

static entry *get_s(dict *d, const char *k)
{
    return dict_get(d, k, strlen(k));
}

static int del_s(dict *d, const char *k)
{
    return dict_del(d, k, strlen(k));
}

static int val_is(const entry *e, const char *v)
{
    return e != NULL && e->vallen == strlen(v) && memcmp(e->val, v, e->vallen) == 0;
}

/* 키를 10진수로 읽어 그대로 해시값으로 쓴다. "5" → 5 → 버킷 5 & mask.
 * 어떤 키가 어느 버킷에 들어갈지 테스트에서 정확히 정할 수 있다. */
static uint64_t num_hash(const char *key, size_t keylen)
{
    uint64_t h = 0;
    for (size_t i = 0; i < keylen; i++)
        h = h * 10 + (uint64_t)(key[i] - '0');
    return h;
}

/* 모든 키가 같은 버킷으로 간다 */
static uint64_t const_hash(const char *key, size_t keylen)
{
    (void)key;
    (void)keylen;
    return 7;
}

/* "0","1","2","3" 을 넣어 버킷 4개를 하나씩 채운 dict. 아직 rehash 전 */
static dict *full_four(void)
{
    dict *d = dict_create_with_hash(num_hash);
    CHECK(d != NULL);
    CHECK(set_s(d, "0", "v0") == 1);
    CHECK(set_s(d, "1", "v1") == 1);
    CHECK(set_s(d, "2", "v2") == 1);
    CHECK(set_s(d, "3", "v3") == 1);
    return d;
}

/* ---- 기본 동작 ---- */

/* TC-HT-001 EP: 넣고 꺼내기 */
static void test_set_then_get(void)
{
    dict *d = dict_create();
    CHECK(set_s(d, "name", "cc8s") == 1);
    CHECK(val_is(get_s(d, "name"), "cc8s"));
    CHECK(dict_len(d) == 1);
    dict_free(d);
}

/* TC-HT-002 EP: 없는 키 */
static void test_get_missing(void)
{
    dict *d = dict_create();
    CHECK(get_s(d, "nope") == NULL);
    set_s(d, "a", "1");
    CHECK(get_s(d, "b") == NULL);
    dict_free(d);
}

/* TC-HT-003 EP: 덮어쓰기는 값만 바뀌고 개수는 그대로 */
static void test_overwrite(void)
{
    dict *d = dict_create();
    CHECK(set_s(d, "k", "old") == 1);
    CHECK(set_s(d, "k", "a much longer new value") == 0);
    CHECK(val_is(get_s(d, "k"), "a much longer new value"));
    CHECK(dict_len(d) == 1);
    dict_free(d);
}

/* TC-HT-004 EP: 삭제 */
static void test_delete(void)
{
    dict *d = dict_create();
    set_s(d, "a", "1");
    set_s(d, "b", "2");
    CHECK(del_s(d, "a") == 1);
    CHECK(get_s(d, "a") == NULL);
    CHECK(val_is(get_s(d, "b"), "2"));
    CHECK(dict_len(d) == 1);
    CHECK(del_s(d, "a") == 0);
    CHECK(del_s(d, "zzz") == 0);
    dict_free(d);
}

/* TC-HT-005 EG: 바이너리 안전. \0 뒤까지 비교해야 한다 */
static void test_binary_safe_keys(void)
{
    dict *d = dict_create();
    CHECK(dict_set(d, "a\0b", 3, "x\0y", 3) == 1);
    CHECK(dict_set(d, "a\0c", 3, "2", 1) == 1);
    CHECK(dict_set(d, "a", 1, "3", 1) == 1);
    CHECK(dict_len(d) == 3);

    entry *e = dict_get(d, "a\0b", 3);
    CHECK(e != NULL && e->vallen == 3 && memcmp(e->val, "x\0y", 3) == 0);
    CHECK(val_is(dict_get(d, "a\0c", 3), "2"));
    CHECK(val_is(dict_get(d, "a", 1), "3"));
    CHECK(dict_get(d, "a\0", 2) == NULL);
    dict_free(d);
}

/* TC-HT-006 BVA: 길이 0인 키와 값 */
static void test_empty_key_and_value(void)
{
    dict *d = dict_create();
    CHECK(dict_set(d, "", 0, "", 0) == 1);
    entry *e = dict_get(d, "", 0);
    CHECK(e != NULL && e->vallen == 0);
    CHECK(dict_del(d, "", 0) == 1);
    CHECK(dict_len(d) == 0);
    dict_free(d);
}

/* TC-HT-007 EG: dict 는 복사본을 가진다 */
static void test_copies_buffers(void)
{
    dict *d = dict_create();
    char k[] = "key";
    char v[] = "val";
    dict_set(d, k, 3, v, 3);
    k[0] = 'X';
    v[0] = 'X';
    CHECK(val_is(get_s(d, "key"), "val"));
    CHECK(get_s(d, "Xey") == NULL);
    dict_free(d);
}

/* TC-HT-008 EG: 모든 키가 한 버킷에 모인 긴 체인. 첫·중간·끝 노드 삭제 */
static void test_long_chain(void)
{
    dict *d = dict_create_with_hash(const_hash);
    char k[16];
    for (int i = 0; i < 100; i++) {
        snprintf(k, sizeof k, "k%d", i);
        CHECK(set_s(d, k, k) == 1);
    }
    dict_rehash_step(d, 1000); /* 진행 중인 rehash 를 끝낸다 */
    CHECK(dict_len(d) == 100);
    for (int i = 0; i < 100; i++) {
        snprintf(k, sizeof k, "k%d", i);
        CHECK(val_is(get_s(d, k), k));
    }
    CHECK(del_s(d, "k0") == 1);
    CHECK(del_s(d, "k50") == 1);
    CHECK(del_s(d, "k99") == 1);
    CHECK(dict_len(d) == 97);
    CHECK(get_s(d, "k0") == NULL);
    CHECK(get_s(d, "k50") == NULL);
    CHECK(get_s(d, "k99") == NULL);
    CHECK(val_is(get_s(d, "k1"), "k1"));
    CHECK(val_is(get_s(d, "k98"), "k98"));
    dict_free(d);
}

/* ---- 확장 시점 ---- */

/* TC-HT-009 BVA: 버킷 4개에 4개까지는 그대로, 5번째 새 키에서 rehash 시작 */
static void test_expand_boundary(void)
{
    dict *d = dict_create_with_hash(num_hash);
    CHECK(d->ht[0].size == DICT_INITIAL_SIZE);
    CHECK(!dict_is_rehashing(d));
    CHECK(dict_len(d) == 0);

    set_s(d, "0", "v0");
    set_s(d, "1", "v1");
    set_s(d, "2", "v2");
    set_s(d, "3", "v3");
    CHECK(!dict_is_rehashing(d));
    CHECK(d->ht[0].size == 4);

    set_s(d, "3", "again"); /* 기존 키 덮어쓰기는 확장을 일으키지 않는다 */
    CHECK(!dict_is_rehashing(d));

    CHECK(set_s(d, "4", "v4") == 1);
    CHECK(dict_is_rehashing(d));
    CHECK(d->rehashidx == 0);    /* 시작한 호출에서는 옮기지 않는다 */
    CHECK(d->ht[1].size == 8);
    CHECK(d->ht[0].used == 4);
    CHECK(d->ht[1].used == 1);   /* 새 키는 ht[1] 로 */
    CHECK(dict_len(d) == 5);
    dict_free(d);
}

/* ---- rehash 도중 (상태 전이) ---- */

/* TC-HT-010 ST: rehash 도중 새 키는 ht[1] 로. 이미 옮긴 버킷 자리의 키도 잃지 않는다 */
static void test_new_key_during_rehash_not_lost(void)
{
    dict *d = full_four();
    set_s(d, "4", "v4");          /* rehash 시작, rehashidx 0 */
    dict_rehash_step(d, 2);       /* 버킷 0, 1 옮김 */
    CHECK(d->rehashidx == 2);
    CHECK(d->ht[0].used == 2);
    CHECK(d->ht[1].used == 3);

    /* "8" 은 ht[0] 기준 버킷 0: 이미 지나간 자리. ht[0] 에 넣으면 유실된다 */
    CHECK(set_s(d, "8", "v8") == 1); /* 먼저 한 칸 옮김(버킷 2) → rehashidx 3 */
    CHECK(d->rehashidx == 3);
    CHECK(d->ht[0].used == 1);
    CHECK(d->ht[1].used == 5);

    dict_rehash_step(d, 100);
    CHECK(!dict_is_rehashing(d));
    CHECK(dict_len(d) == 6);
    CHECK(val_is(get_s(d, "8"), "v8"));
    CHECK(val_is(get_s(d, "0"), "v0"));
    CHECK(val_is(get_s(d, "4"), "v4"));
    dict_free(d);
}

/* TC-HT-011 ST: rehash 도중 기존 키 덮어쓰기는 제자리에서. 중복이 생기면 안 된다 */
static void test_overwrite_during_rehash(void)
{
    dict *d = full_four();
    set_s(d, "4", "v4");               /* rehashidx 0 */
    CHECK(set_s(d, "3", "new3") == 0); /* 한 칸 옮김 → rehashidx 1, "3" 은 ht[0] 버킷 3 에 그대로 */
    CHECK(d->rehashidx == 1);
    CHECK(d->ht[0].used == 3);
    CHECK(d->ht[1].used == 2);
    CHECK(dict_len(d) == 5);

    dict_rehash_step(d, 100);
    CHECK(dict_len(d) == 5);
    CHECK(val_is(get_s(d, "3"), "new3"));
    CHECK(del_s(d, "3") == 1);
    CHECK(get_s(d, "3") == NULL); /* 중복 사본이 남아 있으면 여기서 걸린다 */
    dict_free(d);
}

/* TC-HT-012 DT: rehash 도중 조회·삭제 × 키가 ht[0] / ht[1] 에 있는 경우 */
static void test_get_del_both_tables(void)
{
    dict *d = full_four();
    set_s(d, "4", "v4");                 /* rehashidx 0, "4" 는 ht[1] */
    CHECK(val_is(get_s(d, "3"), "v3"));  /* 옮김 → 1. "3" 은 ht[0] 에서 찾음 */
    CHECK(val_is(get_s(d, "4"), "v4"));  /* 옮김 → 2. "4" 는 ht[1] 에서 찾음 */
    CHECK(d->rehashidx == 2);
    CHECK(del_s(d, "3") == 1);           /* 옮김 → 3. ht[0] 에서 삭제 */
    CHECK(del_s(d, "4") == 1);           /* 옮기면서 rehash 끝, 그 뒤 삭제 */
    CHECK(!dict_is_rehashing(d));
    CHECK(dict_len(d) == 3);
    CHECK(get_s(d, "3") == NULL);
    CHECK(get_s(d, "4") == NULL);
    CHECK(val_is(get_s(d, "0"), "v0"));
    dict_free(d);
}

/* TC-HT-013 ST: rehash 가 끝나면 ht[1] 이 ht[0] 이 되고 ht[1] 은 빈다 */
static void test_rehash_completion_state(void)
{
    dict *d = full_four();
    set_s(d, "4", "v4");
    dict_rehash_step(d, 100); /* 남은 칸보다 큰 n: 넘치지 않고 끝나야 한다 */
    CHECK(!dict_is_rehashing(d));
    CHECK(d->rehashidx == -1);
    CHECK(d->ht[0].size == 8);
    CHECK(d->ht[0].mask == 7);
    CHECK(d->ht[0].used == 5);
    CHECK(d->ht[1].buckets == NULL);
    CHECK(d->ht[1].size == 0);
    CHECK(d->ht[1].used == 0);
    dict_free(d);
}

/* TC-HT-014 ST: rehash 도중에는 set/get/del 한 번마다 한 칸씩 진행한다 */
static void test_each_op_advances_rehash(void)
{
    dict *d = full_four();
    set_s(d, "4", "v4");
    CHECK(d->rehashidx == 0);
    get_s(d, "0");
    CHECK(d->rehashidx == 1);
    get_s(d, "99");               /* 없는 키 조회도 진행시킨다 */
    CHECK(d->rehashidx == 2);
    del_s(d, "77");               /* 없는 키 삭제도 */
    CHECK(d->rehashidx == 3);
    CHECK(d->ht[0].used == 1);
    get_s(d, "3");                /* 마지막 키를 옮기면 끝 */
    CHECK(!dict_is_rehashing(d));
    CHECK(dict_len(d) == 5);
    dict_free(d);
}

/* TC-HT-015 EG: rehash 중이 아닐 때 dict_rehash_step 은 아무것도 안 한다 */
static void test_step_when_not_rehashing(void)
{
    dict *d = full_four();
    dict_rehash_step(d, 5);
    CHECK(!dict_is_rehashing(d));
    CHECK(d->ht[0].size == 4);
    CHECK(dict_len(d) == 4);
    dict_free(d);
    dict_free(NULL);
}

/* TC-HT-016 EP: 키 1만 개. 여러 번 확장을 거쳐도 전부 남고, 전부 지울 수 있다 */
static void test_many_keys(void)
{
    dict *d = dict_create();
    char k[32];
    for (int i = 0; i < 10000; i++) {
        snprintf(k, sizeof k, "key:%d", i);
        CHECK(set_s(d, k, k) == 1);
    }
    CHECK(dict_len(d) == 10000);
    for (int i = 0; i < 10000; i++) {
        snprintf(k, sizeof k, "key:%d", i);
        CHECK(val_is(get_s(d, k), k));
    }
    for (int i = 0; i < 10000; i++) {
        snprintf(k, sizeof k, "key:%d", i);
        CHECK(del_s(d, k) == 1);
    }
    CHECK(dict_len(d) == 0);
    dict_free(d);
}

/* ---- 무작위 엔트리 (active 만료 샘플링용) ---- */

/* 0,1,2,... 를 차례로 돌려주는 난수. 어떤 버킷·노드가 뽑힐지 정할 수 있다 */
static int seq_next = 0;
static int seq_rand(void)
{
    return seq_next++;
}

/* 키 이름(10진수 한 자리)으로 seen[] 에 표시 */
static void mark(int *seen, const entry *e)
{
    CHECK(e != NULL);
    CHECK(e->keylen == 1 && e->key[0] >= '0' && e->key[0] <= '9');
    seen[e->key[0] - '0']++;
}

/* TC-HT-017 EP: 빈 테이블에서는 NULL */
static void test_random_empty(void)
{
    dict *d = dict_create();
    CHECK(dict_random_entry(d) == NULL);
    set_s(d, "a", "1");
    del_s(d, "a");
    CHECK(dict_random_entry(d) == NULL);
    dict_free(d);
}

/* TC-HT-018 EP: 있는 키만 나오고, 여러 번 뽑으면 모든 키가 나온다 */
static void test_random_covers_all(void)
{
    dict_set_rand(NULL); /* 진짜 rand */
    srand(1);
    dict *d = dict_create();
    char k[2] = "0";
    for (int i = 0; i < 10; i++) {
        k[0] = (char)('0' + i);
        set_s(d, k, k);
    }
    int seen[10] = {0};
    for (int i = 0; i < 2000; i++)
        mark(seen, dict_random_entry(d));
    for (int i = 0; i < 10; i++)
        CHECK(seen[i] > 0);
    dict_free(d);
}

/* TC-HT-019 ST: rehash 도중에도 두 테이블의 키가 모두 나오고, rehash 는 진행시키지 않는다 */
static void test_random_during_rehash(void)
{
    dict *d = full_four();
    set_s(d, "4", "v4");
    dict_rehash_step(d, 2); /* ht[0]: "2","3" / ht[1]: "4","0","1" */
    CHECK(d->rehashidx == 2);

    /* 순차 난수(seq_rand)는 쓰지 않는다. 한 번 뽑을 때 버킷·체인에 하나씩 쓰여 짝·홀이 갈리고,
     * 버킷 위치가 10칸 주기라 홀수 위치 버킷("3", "1")은 영원히 안 뽑힌다(처음 이 테스트가 그렇게 실패함). */
    dict_set_rand(NULL);
    srand(2);
    int seen[10] = {0};
    for (int i = 0; i < 2000; i++)
        mark(seen, dict_random_entry(d));
    for (int i = 0; i <= 4; i++)
        CHECK(seen[i] > 0);
    for (int i = 5; i < 10; i++)
        CHECK(seen[i] == 0);
    CHECK(d->rehashidx == 2); /* 진행시키지 않음 */
    CHECK(dict_len(d) == 5);
    dict_free(d);
}

/* TC-HT-020 EG: 한 체인에 여러 노드가 있으면 체인 안에서도 고른다 (첫 노드만 나오면 안 됨) */
static void test_random_within_chain(void)
{
    dict *d = dict_create_with_hash(const_hash); /* 전부 한 버킷 */
    set_s(d, "1", "1");
    set_s(d, "2", "2");
    set_s(d, "3", "3");
    dict_set_rand(seq_rand);
    seq_next = 0;
    int seen[10] = {0};
    for (int i = 0; i < 60; i++)
        mark(seen, dict_random_entry(d));
    CHECK(seen[1] > 0 && seen[2] > 0 && seen[3] > 0);
    dict_set_rand(NULL);
    dict_free(d);
}

/* ---- 순회 (KEYS 용) ---- */

/* TC-HT-021 EP: 키 1000개를 정확히 한 번씩 */
static void test_iter_all_once(void)
{
    dict *d = dict_create();
    char k[16];
    for (int i = 0; i < 1000; i++) {
        snprintf(k, sizeof k, "%d", i);
        set_s(d, k, "v");
    }
    static int seen[1000];
    memset(seen, 0, sizeof seen);
    dict_iter it;
    dict_iter_init(&it, d);
    int n = 0;
    for (entry *e = dict_iter_next(&it); e != NULL; e = dict_iter_next(&it)) {
        char buf[16];
        memcpy(buf, e->key, e->keylen);
        buf[e->keylen] = '\0';
        int i = atoi(buf);
        CHECK(i >= 0 && i < 1000);
        seen[i]++;
        n++;
    }
    CHECK(n == 1000);
    for (int i = 0; i < 1000; i++)
        CHECK(seen[i] == 1);
    dict_free(d);
}

/* TC-HT-022 ST: rehash 도중에는 두 테이블을 합쳐 정확히 한 번씩, rehash 는 진행시키지 않는다 */
static void test_iter_during_rehash(void)
{
    dict *d = full_four();
    set_s(d, "4", "v4");
    dict_rehash_step(d, 1); /* ht[0]: "1","2","3" / ht[1]: "4","0" */
    int seen[10] = {0};
    dict_iter it;
    dict_iter_init(&it, d);
    for (entry *e = dict_iter_next(&it); e != NULL; e = dict_iter_next(&it))
        mark(seen, e);
    for (int i = 0; i <= 4; i++)
        CHECK(seen[i] == 1);
    CHECK(d->rehashidx == 1);
    dict_free(d);
}

/* TC-HT-023 BVA: 빈 테이블 순회는 바로 끝나고, 끝난 뒤 다시 불러도 NULL */
static void test_iter_empty(void)
{
    dict *d = dict_create();
    dict_iter it;
    dict_iter_init(&it, d);
    CHECK(dict_iter_next(&it) == NULL);
    CHECK(dict_iter_next(&it) == NULL);
    dict_free(d);
}

/* ---- 전체 삭제 (FLUSHALL 용) ---- */

/* TC-HT-024 ST: rehash 도중에 비워도 처음 상태로 돌아가고, 다시 쓸 수 있다 */
static void test_clear(void)
{
    dict *d = dict_create();
    char k[16];
    for (int i = 0; i < 100; i++) {
        snprintf(k, sizeof k, "k%d", i);
        set_s(d, k, k);
    }
    CHECK(dict_is_rehashing(d) || d->ht[0].size > DICT_INITIAL_SIZE);
    CHECK(dict_clear(d) == 0);
    CHECK(dict_len(d) == 0);
    CHECK(!dict_is_rehashing(d));
    CHECK(d->ht[0].size == DICT_INITIAL_SIZE);
    CHECK(d->ht[1].buckets == NULL && d->ht[1].size == 0);
    CHECK(get_s(d, "k1") == NULL);
    CHECK(dict_random_entry(d) == NULL);

    CHECK(set_s(d, "again", "1") == 1);
    CHECK(val_is(get_s(d, "again"), "1"));
    CHECK(dict_len(d) == 1);
    CHECK(dict_clear(d) == 0);
    CHECK(dict_clear(d) == 0); /* 빈 테이블을 또 비워도 됨 */
    dict_free(d);
}

int main(void)
{
    RUN(test_set_then_get);
    RUN(test_get_missing);
    RUN(test_overwrite);
    RUN(test_delete);
    RUN(test_binary_safe_keys);
    RUN(test_empty_key_and_value);
    RUN(test_copies_buffers);
    RUN(test_long_chain);
    RUN(test_expand_boundary);
    RUN(test_new_key_during_rehash_not_lost);
    RUN(test_overwrite_during_rehash);
    RUN(test_get_del_both_tables);
    RUN(test_rehash_completion_state);
    RUN(test_each_op_advances_rehash);
    RUN(test_step_when_not_rehashing);
    RUN(test_many_keys);
    RUN(test_random_empty);
    RUN(test_random_covers_all);
    RUN(test_random_during_rehash);
    RUN(test_random_within_chain);
    RUN(test_iter_all_once);
    RUN(test_iter_during_rehash);
    RUN(test_iter_empty);
    RUN(test_clear);
    TEST_DONE();
    return 0;
}
