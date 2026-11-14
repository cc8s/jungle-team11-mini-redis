#ifndef MINI_REDIS_DB_H
#define MINI_REDIS_DB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "str.h"

enum {
    DB_FREE_CHUNK_BUCKETS = 100,
};

enum db_result {
    DB_OK = 0,
    DB_NOT_FOUND,
    DB_OUT_OF_MEMORY,
};

struct db;

/* 빈 저장소를 만든다. 실패하면 NULL, db_destroy() 로 해제한다. */
struct db *db_create(void);

/* 모든 키 · 값 · 만료 시각을 해제하고 저장소를 해제한다. NULL 이면 아무것도 하지 않는다. */
void db_destroy(struct db *db);

/* 키와 값을 가져가 저장하고 기존 TTL 을 지운다. 실패하면 소유권은 호출자에게 남는다. */
enum db_result db_set(struct db *db, struct str *key, struct str *value);

/* 값을 반환한다. 없거나 만료됐으면 NULL 이고, 만료된 키는 이때 지운다. 값은 db 가 소유한다. */
const struct str *db_get(struct db *db, const struct str *key, int64_t now_ms);

/* 있는 키에 절대 만료 시각을 건다. now_ms >= expire_at_ms 가 되면 만료된 것으로 본다. */
enum db_result db_set_expire(struct db *db, const struct str *key, int64_t expire_at_ms);

/* 키를 지운다. 없거나 이미 만료된 키면 DB_NOT_FOUND 이다. */
enum db_result db_delete(struct db *db, const struct str *key, int64_t now_ms);

/* 만료된 키만 지운다. 지웠으면 DB_OK, 없거나 아직 만료 전이면 DB_NOT_FOUND 이다. */
enum db_result db_delete_if_expired(struct db *db, const struct str *key, int64_t now_ms);

/* TTL 이 있는 키 하나를 무작위로 반환한다. 없거나 못 찾으면 NULL.
 * 키는 db 가 소유하며 다음 변경 연산까지만 유효하다. */
const struct str *db_random_ttl_key(const struct db *db);

/* 모든 키를 지운다. 이전 테이블은 대기 목록으로 옮기고 db_free_pending_for_ms() 가 해제한다.
 * 할당이 실패하면 아무것도 바꾸지 않고 DB_OUT_OF_MEMORY 이다. */
enum db_result db_flush(struct db *db);

/* 대기 목록의 테이블을 budget_ms 동안 해제한다. 예산이 0 이어도 버킷 한 묶음은 해제한다. */
void db_free_pending_for_ms(struct db *db, int64_t budget_ms);

/* 해제를 기다리는 테이블이 있으면 true 를 반환한다. */
bool db_has_pending_free(const struct db *db);

/* 두 테이블의 rehash 를 각각 budget_ms 동안 진행한다. 주기 작업에서 부른다. */
void db_rehash_for_ms(struct db *db, int64_t budget_ms);

/* 저장된 키 수를 반환한다. 만료됐지만 아직 지우지 않은 키도 센다. */
size_t db_count(const struct db *db);

/* TTL 이 있는 키 수를 반환한다. */
size_t db_ttl_count(const struct db *db);

#endif
