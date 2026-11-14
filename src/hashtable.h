#ifndef HASHTABLE_H
#define HASHTABLE_H

/* 체이닝 + 점진적 rehash 해시 테이블.
 *
 * 핵심 불변식: rehash 도중에도 모든 키는 ht[0], ht[1] 중 정확히 한 곳에만 있다.
 *
 * 동작 계약
 *  - 키와 값은 바이너리 안전(\0 포함 가능)이다. 길이를 따로 받는다.
 *  - dict 는 키와 값을 복사해서 가진다. 호출자 버퍼를 나중에 바꿔도 영향이 없다.
 *  - 처음 버킷 수는 DICT_INITIAL_SIZE 이고, 버킷 수는 항상 2의 거듭제곱이다.
 *  - dict_set / dict_get / dict_del 은 호출 시점에 rehash 중이면 먼저 dict_rehash_step(d, 1) 을 한다.
 *  - dict_set 으로 새 키를 넣을 때, rehash 중이 아니고 ht[0].used >= ht[0].size 이면
 *    크기 2배의 ht[1] 을 만들고 rehashidx = 0 으로 rehash 를 시작한다(이 호출에서는 옮기지 않는다).
 *  - rehash 중에 들어오는 새 키는 항상 ht[1] 에 넣는다.
 *  - 이미 있는 키를 dict_set 하면 그 키가 있는 자리에서 값만 바꾼다.
 *  - dict_rehash_step(d, n): ht[0] 의 버킷을 rehashidx 부터 최대 n 개(빈 버킷 포함) 옮긴다.
 *    ht[0].used 가 0 이 되면 ht[1] 을 ht[0] 으로 바꾸고, ht[1] 을 비우고, rehashidx = -1 로 끝낸다.
 *    rehash 중이 아니면 아무것도 하지 않는다. */

#include <stddef.h>
#include <stdint.h>

#define DICT_INITIAL_SIZE 4

typedef uint64_t (*dict_hash_fn)(const char *key, size_t keylen);

/* 체인의 노드 하나 */
typedef struct entry {
    char *key;
    size_t keylen;
    char *val;
    size_t vallen;
    struct entry *next;
} entry;

/* 테이블 하나 (ht[0] 또는 ht[1]). 비어 있는 ht[1] 은 buckets = NULL, size = 0 */
typedef struct {
    entry **buckets;
    size_t size; /* 버킷 수, 2의 거듭제곱 */
    size_t mask; /* size - 1 */
    size_t used; /* 들어 있는 키 수 */
} table;

typedef struct {
    table ht[2];
    long rehashidx; /* -1 = rehash 아님, 0 이상 = 다음에 옮길 ht[0] 버킷 */
    dict_hash_fn hash;
} dict;

#define dict_is_rehashing(d) ((d)->rehashidx != -1)

/* 기본 해시 함수(FNV-1a 64비트) */
uint64_t dict_default_hash(const char *key, size_t keylen);

dict *dict_create(void);
/* 테스트에서 충돌을 일부러 만들 때 쓴다 */
dict *dict_create_with_hash(dict_hash_fn hash);
void dict_free(dict *d); /* NULL 이면 아무것도 하지 않음 */

/* 새 키를 넣으면 1, 기존 키의 값을 바꾸면 0, 메모리 할당 실패면 -1 */
int dict_set(dict *d, const char *key, size_t keylen, const char *val, size_t vallen);
/* 없으면 NULL. 돌려받은 entry 는 다음 dict_set / dict_del 전까지만 유효하다 */
entry *dict_get(dict *d, const char *key, size_t keylen);
/* 지웠으면 1, 없으면 0 */
int dict_del(dict *d, const char *key, size_t keylen);
size_t dict_len(const dict *d);
void dict_rehash_step(dict *d, int n);

/* 무작위 엔트리 하나 (active 만료 샘플링용). 비어 있으면 NULL.
 * 키가 있는 버킷을 무작위로 고른 뒤 그 체인에서 무작위로 고른다. 체인 길이가 다르면 완전히 고르지는 않다(Redis 와 같음).
 * rehash 를 진행시키지 않는다. 난수는 dict_set_rand 로 바꿔 끼울 수 있다(NULL 이면 rand). */
entry *dict_random_entry(dict *d);
void dict_set_rand(int (*rand_fn)(void));

/* 순회 (KEYS 용). 모든 키를 정확히 한 번씩 돌려준다. rehash 도중이면 ht[0] 다음 ht[1].
 * 순회 중에는 dict_set / dict_get / dict_del 을 부르지 않는다(rehash 가 진행되어 엔트리가 옮겨진다). */
typedef struct {
    dict *d;
    int table;
    size_t bucket;
    entry *next;
} dict_iter;
void dict_iter_init(dict_iter *it, dict *d);
entry *dict_iter_next(dict_iter *it); /* 끝이면 NULL */

/* 전부 지우고 처음 상태(버킷 DICT_INITIAL_SIZE 개, rehash 아님)로 (FLUSHALL 용). 실패하면 -1 */
int dict_clear(dict *d);

#endif
