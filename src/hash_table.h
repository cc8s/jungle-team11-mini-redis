#ifndef MINI_REDIS_HASH_TABLE_H
#define MINI_REDIS_HASH_TABLE_H

#include <stddef.h>
#include <stdint.h>

#include "str.h"

enum {
    HT_INITIAL_SIZE = 16,
    HT_RANDOM_MAX_PROBES = 100,
    HT_SEED_BYTES = 16,
};

enum ht_result {
    HT_OK = 0,
    HT_NOT_FOUND,
    HT_OUT_OF_MEMORY,
    HT_IO_ERROR,
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

/* 키와 값을 가져가 저장한다(같은 키면 이전 값 해제). 실패하면 소유권은 호출자에게 남는다. */
enum ht_result hash_table_set(struct hash_table *ht, struct str *key, void *value);

/* key 와 바이트가 같은 엔트리를 반환한다. 없으면 NULL. 엔트리는 테이블이 소유한다. */
const struct ht_entry *hash_table_find(const struct hash_table *ht, const struct str *key);

/* key 와 같은 엔트리를 지우고 그 키와 값을 해제한다. 없으면 HT_NOT_FOUND. */
enum ht_result hash_table_delete(struct hash_table *ht, const struct str *key);

/* 무작위 엔트리 하나를 반환한다. 비었거나 빈 버킷만 HT_RANDOM_MAX_PROBES 번 만나면 NULL. */
const struct ht_entry *hash_table_random_entry(const struct hash_table *ht);

/* 저장된 엔트리 수를 반환한다. */
size_t hash_table_count(const struct hash_table *ht);

/* 버킷 배열의 크기를 반환한다. */
size_t hash_table_bucket_count(const struct hash_table *ht);

/* 키의 모든 바이트로 seed 를 넣은 SipHash-2-4 를 계산한다. 같은 seed 면 같은 값이다. */
uint64_t hash_table_hash(const struct str *key);

/* 해시 seed 를 바꾼다. 기존 테이블의 칸이 어긋나므로 테이블을 만들기 전에만 부른다. */
void hash_table_set_seed(const uint8_t seed[HT_SEED_BYTES]);

/* /dev/urandom 에서 seed 를 읽어 설정한다. 서버 시작 시 한 번 부른다. */
enum ht_result hash_table_load_random_seed(void);

#endif
