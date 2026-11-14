#ifndef MINI_REDIS_DB_H
#define MINI_REDIS_DB_H

#include <stddef.h>
#include <stdint.h>

#include "str.h"

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

/* 저장된 키 수를 반환한다. 만료됐지만 아직 지우지 않은 키도 센다. */
size_t db_count(const struct db *db);

#endif
