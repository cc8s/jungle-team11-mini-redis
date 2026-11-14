#include "db.h"

#include <stdbool.h>

#include "clock.h"
#include "hash_table.h"
#include "mem.h"

/* flush 로 떼어 낸 테이블. 밖에서는 보이지 않고 hash_table_destroy_step 으로만 줄어든다 */
struct pending_table {
    struct hash_table *ht;
    struct pending_table *next;
};

/* keys 테이블의 값. data 는 이 구조체가 소유한다 */
struct db_value {
    struct str *data;
    int64_t last_access_ms;
};

/* 불변식: expires 의 키는 항상 keys 에도 있다. TTL 이 없는 키는 expires 에 없다. */
struct db {
    struct hash_table *keys;    /* 값: struct db_value * */
    struct hash_table *expires; /* 값: int64_t * (expire_at_ms) */
    struct pending_table *pending;
    size_t max_bytes; /* 0 이면 제한 없음 */
};

static enum db_result make_room(struct db *db, const struct db_set_request *req);
static bool free_pending_chunk(struct db *db);
static void evict_one(struct db *db, int64_t now_ms);
static const struct str *pick_eviction_victim(const struct db *db, int64_t now_ms);
static struct pending_table *push_pending(struct pending_table *node, struct hash_table *ht,
                                          struct pending_table *next);
static struct hash_table *create_keys_table(void);
static void destroy_db_value(void *value);
static bool is_expired(const struct db *db, const struct str *key, int64_t now_ms);
static void delete_key(struct db *db, const struct str *key);
static void evict_key(struct db *db, const struct str *key);

struct db *db_create(void)
{
    struct db *db = mem_alloc(sizeof(*db));
    if (db == NULL) {
        return NULL;
    }
    db->pending = NULL;
    db->max_bytes = 0;
    db->keys = create_keys_table();
    db->expires = hash_table_create(mem_free);
    if (db->keys == NULL || db->expires == NULL) {
        db_destroy(db);
        return NULL;
    }
    return db;
}

void db_destroy(struct db *db)
{
    if (db == NULL) {
        return;
    }
    while (db->pending != NULL) {
        struct pending_table *next = db->pending->next;
        hash_table_destroy(db->pending->ht);
        mem_free(db->pending);
        db->pending = next;
    }
    hash_table_destroy(db->expires);
    hash_table_destroy(db->keys);
    mem_free(db);
}

void db_set_max_bytes(struct db *db, size_t max_bytes)
{
    db->max_bytes = max_bytes;
}

enum db_result db_set(struct db *db, const struct db_set_request *req)
{
    if (make_room(db, req) != DB_OK) {
        return DB_OUT_OF_MEMORY;
    }
    struct db_value *value = mem_alloc(sizeof(*value));
    if (value == NULL) {
        return DB_OUT_OF_MEMORY;
    }
    value->data = req->value;
    value->last_access_ms = req->now_ms;

    /* TTL 을 먼저 지워도 안전하다. 키가 없으면 불변식상 expires 에도 없어서 지울 것이 없고,
     * 키가 있으면 hash_table_set 은 값만 바꾸므로 할당하지 않아 실패하지 않는다. */
    hash_table_delete(db->expires, req->key);
    if (hash_table_set(db->keys, req->key, value) != HT_OK) {
        mem_free(value); /* data 는 호출자 소유로 남긴다 */
        return DB_OUT_OF_MEMORY;
    }
    return DB_OK;
}

const struct str *db_get(struct db *db, const struct str *key, int64_t now_ms)
{
    if (is_expired(db, key, now_ms)) {
        delete_key(db, key);
        return NULL;
    }
    const struct ht_entry *entry = hash_table_find(db->keys, key);
    if (entry == NULL) {
        return NULL;
    }
    struct db_value *value = entry->value;
    value->last_access_ms = now_ms;
    return value->data;
}

enum db_result db_find_last_access_ms(const struct db *db, const struct str *key,
                                      int64_t *out_last_access_ms)
{
    const struct ht_entry *entry = hash_table_find(db->keys, key);
    if (entry == NULL) {
        return DB_NOT_FOUND;
    }
    const struct db_value *value = entry->value;
    *out_last_access_ms = value->last_access_ms;
    return DB_OK;
}

enum db_result db_set_expire(struct db *db, const struct str *key, int64_t expire_at_ms)
{
    if (hash_table_find(db->keys, key) == NULL) {
        return DB_NOT_FOUND;
    }

    struct str *key_copy = str_dup(key);
    int64_t *expire_at = mem_alloc(sizeof(*expire_at));
    if (key_copy == NULL || expire_at == NULL) {
        goto cleanup;
    }
    *expire_at = expire_at_ms;
    if (hash_table_set(db->expires, key_copy, expire_at) == HT_OK) {
        return DB_OK; /* 키 복사본과 만료 시각의 소유권이 expires 로 넘어갔다 */
    }

cleanup:
    mem_free(expire_at);
    str_destroy(key_copy);
    return DB_OUT_OF_MEMORY;
}

enum db_result db_delete(struct db *db, const struct str *key, int64_t now_ms)
{
    if (hash_table_find(db->keys, key) == NULL) {
        return DB_NOT_FOUND;
    }
    bool was_expired = is_expired(db, key, now_ms);
    delete_key(db, key);
    return was_expired ? DB_NOT_FOUND : DB_OK;
}

enum db_result db_delete_if_expired(struct db *db, const struct str *key, int64_t now_ms)
{
    if (!is_expired(db, key, now_ms)) {
        return DB_NOT_FOUND;
    }
    delete_key(db, key);
    return DB_OK;
}

const struct str *db_random_ttl_key(const struct db *db)
{
    const struct ht_entry *entry = hash_table_random_entry(db->expires);
    if (entry == NULL) {
        return NULL;
    }
    return entry->key;
}

void db_rehash_for_ms(struct db *db, int64_t budget_ms)
{
    hash_table_rehash_for_ms(db->keys, budget_ms);
    hash_table_rehash_for_ms(db->expires, budget_ms);
}

/* 이전 두 테이블을 대기 목록으로 옮기기만 해서 키 수와 상관없이 바로 끝난다.
 * 필요한 것을 모두 할당한 뒤에 바꾸므로 실패하면 db 는 그대로다 */
enum db_result db_flush(struct db *db)
{
    struct hash_table *new_keys = create_keys_table();
    struct hash_table *new_expires = hash_table_create(mem_free);
    struct pending_table *keys_node = mem_alloc(sizeof(*keys_node));
    struct pending_table *expires_node = mem_alloc(sizeof(*expires_node));
    if (new_keys == NULL || new_expires == NULL || keys_node == NULL || expires_node == NULL) {
        goto cleanup;
    }

    db->pending = push_pending(keys_node, db->keys, db->pending);
    db->pending = push_pending(expires_node, db->expires, db->pending);
    db->keys = new_keys;
    db->expires = new_expires;
    return DB_OK;

cleanup:
    mem_free(expires_node);
    mem_free(keys_node);
    hash_table_destroy(new_expires);
    hash_table_destroy(new_keys);
    return DB_OUT_OF_MEMORY;
}

/* 시계를 매 버킷마다 읽지 않도록 한 묶음을 해제한 뒤에만 경과 시간을 확인한다 */
void db_free_pending_for_ms(struct db *db, int64_t budget_ms)
{
    int64_t start_ms = clock_now_ms();
    while (db->pending != NULL) {
        if (hash_table_destroy_step(db->pending->ht, DB_FREE_CHUNK_BUCKETS)) {
            struct pending_table *next = db->pending->next;
            mem_free(db->pending);
            db->pending = next;
        }
        if (clock_now_ms() - start_ms >= budget_ms) {
            return;
        }
    }
}

bool db_has_pending_free(const struct db *db)
{
    return db->pending != NULL;
}

/* 해제 대기 중인 테이블이 있으면 그것부터 줄인다. flush 직후에는 논리적으로 비었는데도
 * 사용량이 그대로라 새 키를 퇴출하게 되기 때문이다 */
enum db_result db_evict_until_under(struct db *db, size_t max_bytes, int64_t now_ms)
{
    while (mem_used_bytes() > max_bytes) {
        if (free_pending_chunk(db)) {
            continue;
        }
        if (hash_table_count(db->keys) == 0) {
            return DB_OUT_OF_MEMORY;
        }
        evict_one(db, now_ms);
    }
    return DB_OK;
}

size_t db_count(const struct db *db)
{
    return hash_table_count(db->keys);
}

size_t db_ttl_count(const struct db *db)
{
    return hash_table_count(db->expires);
}

/* 값 하나가 한도보다 크면 퇴출해도 들어갈 수 없으므로 기존 키를 지우지 않고 바로 실패한다.
 * 키와 값 문자열은 이미 할당돼 사용량에 들어 있으니, 그만큼을 넘지 않을 때까지만 퇴출한다 */
static enum db_result make_room(struct db *db, const struct db_set_request *req)
{
    if (db->max_bytes == 0) {
        return DB_OK;
    }
    size_t incoming_bytes = req->key->len + req->value->len + DB_ENTRY_OVERHEAD_BYTES;
    if (incoming_bytes > db->max_bytes) {
        return DB_OUT_OF_MEMORY;
    }
    return db_evict_until_under(db, db->max_bytes, req->now_ms);
}

/* 대기 목록을 한 묶음 해제했으면 true, 대기 목록이 비었으면 false */
static bool free_pending_chunk(struct db *db)
{
    if (db->pending == NULL) {
        return false;
    }
    if (hash_table_destroy_step(db->pending->ht, DB_FREE_CHUNK_BUCKETS)) {
        struct pending_table *next = db->pending->next;
        mem_free(db->pending);
        db->pending = next;
    }
    return true;
}

/* 표본을 하나도 못 뽑으면 테이블이 성긴 것이므로 rehash 로 줄여서 다음에 잘 뽑히게 한다 */
static void evict_one(struct db *db, int64_t now_ms)
{
    const struct str *victim = pick_eviction_victim(db, now_ms);
    if (victim == NULL) {
        hash_table_rehash_step(db->keys, DB_FREE_CHUNK_BUCKETS);
        return;
    }
    evict_key(db, victim);
}

/* 근사 LRU: 키 DB_EVICT_SAMPLE_COUNT 개를 뽑아 가장 오래 안 쓴 키를 고른다.
 * 만료된 키가 걸리면 어차피 지울 키라 바로 그것을 고른다. 반환된 키는 keys 가 소유한다 */
static const struct str *pick_eviction_victim(const struct db *db, int64_t now_ms)
{
    const struct ht_entry *oldest = NULL;
    for (int i = 0; i < DB_EVICT_SAMPLE_COUNT; i++) {
        const struct ht_entry *entry = hash_table_random_entry(db->keys);
        if (entry == NULL) {
            continue;
        }
        if (is_expired(db, entry->key, now_ms)) {
            return entry->key;
        }
        const struct db_value *value = entry->value;
        if (oldest == NULL ||
            value->last_access_ms < ((const struct db_value *)oldest->value)->last_access_ms) {
            oldest = entry;
        }
    }
    return oldest == NULL ? NULL : oldest->key;
}

static struct pending_table *push_pending(struct pending_table *node, struct hash_table *ht,
                                          struct pending_table *next)
{
    node->ht = ht;
    node->next = next;
    return node;
}

static struct hash_table *create_keys_table(void)
{
    return hash_table_create(destroy_db_value);
}

static void destroy_db_value(void *value)
{
    struct db_value *db_value = value;
    str_destroy(db_value->data);
    mem_free(db_value);
}

/* 경계 정책: 만료 시각과 현재 시각이 같으면 이미 만료된 것으로 본다 (Redis 와 동일) */
static bool is_expired(const struct db *db, const struct str *key, int64_t now_ms)
{
    const struct ht_entry *entry = hash_table_find(db->expires, key);
    if (entry == NULL) {
        return false;
    }
    const int64_t *expire_at_ms = entry->value;
    return now_ms >= *expire_at_ms;
}

/* 두 테이블에서 모두 지운다. key 가 db_random_ttl_key() 가 준 expires 소유의 키일 수 있으므로
 * keys 를 먼저 지우고, 그 키를 해제하는 expires 삭제를 마지막에 한다 */
static void delete_key(struct db *db, const struct str *key)
{
    hash_table_delete(db->keys, key);
    hash_table_delete(db->expires, key);
}

/* delete_key 와 순서가 반대다. key 가 keys 소유라서 keys 를 먼저 지우면 해제된 키로
 * expires 를 찾게 된다 */
static void evict_key(struct db *db, const struct str *key)
{
    hash_table_delete(db->expires, key);
    hash_table_delete(db->keys, key);
}
