#ifndef MINI_REDIS_HASH_TABLE_H
#define MINI_REDIS_HASH_TABLE_H

#include <stddef.h>
#include <stdint.h>

#include "str.h"

enum {
    HT_INITIAL_SIZE = 16,
};

/* 테이블이 값을 해제할 때 부르는 함수. 값을 해제하지 않으려면 NULL 을 넘긴다. */
typedef void ht_value_destroy_fn(void *value);

/* 버킷 체인의 노드. key 와 value 는 테이블이 소유하고, next 는 테이블만 바꾼다. */
struct ht_entry {
    struct str *key;
    void *value;
    struct ht_entry *next;
};

struct hash_table;

/* 빈 테이블을 만든다. 실패하면 NULL, hash_table_destroy() 로 해제한다. */
struct hash_table *hash_table_create(ht_value_destroy_fn *value_destroy);

/* 모든 엔트리의 키와 값을 해제하고 테이블을 해제한다. NULL 이면 아무것도 하지 않는다. */
void hash_table_destroy(struct hash_table *ht);

/* 저장된 엔트리 수를 반환한다. */
size_t hash_table_count(const struct hash_table *ht);

/* 버킷 배열의 크기를 반환한다. */
size_t hash_table_bucket_count(const struct hash_table *ht);

/* 키의 모든 바이트로 64bit 해시를 계산한다. 같은 키는 항상 같은 값이다. */
uint64_t hash_table_hash(const struct str *key);

#endif
