#include "db.h"

#include <stdbool.h>

#include "hash_table.h"
#include "mem.h"

/* 불변식: expires 의 키는 항상 keys 에도 있다. TTL 이 없는 키는 expires 에 없다. */
struct db {
    struct hash_table *keys;    /* 값: struct str * */
    struct hash_table *expires; /* 값: int64_t * (expire_at_ms) */
};

static void destroy_str_value(void *value);
static bool is_expired(const struct db *db, const struct str *key, int64_t now_ms);
static void delete_key(struct db *db, const struct str *key);

struct db *db_create(void)
{
    struct db *db = mem_alloc(sizeof(*db));
    if (db == NULL) {
        return NULL;
    }
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

size_t db_count(const struct db *db)
{
    return hash_table_count(db->keys);
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

/* 두 테이블에서 모두 지운다. expires 를 먼저 지워 불변식이 중간에도 깨지지 않게 한다 */
static void delete_key(struct db *db, const struct str *key)
{
    hash_table_delete(db->expires, key);
    hash_table_delete(db->keys, key);
}
