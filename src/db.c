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

/* 불변식: expires 의 키는 항상 keys 에도 있다. TTL 이 없는 키는 expires 에 없다. */
struct db {
    struct hash_table *keys;    /* 값: struct str * */
    struct hash_table *expires; /* 값: int64_t * (expire_at_ms) */
    struct pending_table *pending;
};

static struct pending_table *push_pending(struct pending_table *node, struct hash_table *ht,
                                          struct pending_table *next);
static void destroy_str_value(void *value);
static bool is_expired(const struct db *db, const struct str *key, int64_t now_ms);
static void delete_key(struct db *db, const struct str *key);

struct db *db_create(void)
{
    struct db *db = mem_alloc(sizeof(*db));
    if (db == NULL) {
        return NULL;
    }
    db->pending = NULL;
    db->keys = hash_table_create(destroy_str_value);
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

enum db_result db_set(struct db *db, struct str *key, struct str *value)
{
    /* TTL 을 먼저 지워도 안전하다. 키가 없으면 불변식상 expires 에도 없어서 지울 것이 없고,
     * 키가 있으면 hash_table_set 은 값만 바꾸므로 할당하지 않아 실패하지 않는다. */
    hash_table_delete(db->expires, key);
    if (hash_table_set(db->keys, key, value) != HT_OK) {
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
    return entry->value;
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
    struct hash_table *new_keys = hash_table_create(destroy_str_value);
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

size_t db_count(const struct db *db)
{
    return hash_table_count(db->keys);
}

size_t db_ttl_count(const struct db *db)
{
    return hash_table_count(db->expires);
}

static struct pending_table *push_pending(struct pending_table *node, struct hash_table *ht,
                                          struct pending_table *next)
{
    node->ht = ht;
    node->next = next;
    return node;
}

static void destroy_str_value(void *value)
{
    str_destroy(value);
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
