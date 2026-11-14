#ifndef DB_H
#define DB_H

/* 키 공간: 값(main)과 만료 시각(expires)을 따로 둔 해시 테이블 두 개 (R007 D1).
 *
 * 만료 규칙 (진짜 Redis 와 같음)
 *  - 만료 시각 when(ms, 절대 시각)이 있는 키는 now_ms() > when 이면 만료다(같을 때는 아직 살아 있다).
 *  - lazy: db_lookup 은 만료된 키를 지우고 NULL 을 돌려준다. 다른 함수도 키를 볼 때 이 규칙을 따른다.
 *  - db_key_expired 는 expires 만 보고 만료 여부를 알려 줄 뿐 지우지 않는다. main 을 건드리지 않으므로
 *    KEYS 가 main 을 순회하는 도중에 불러도 된다(main 을 조회하면 rehash 가 진행돼 순회 계약을 어긴다).
 *  - db_set 은 기존 TTL 을 지운다(SET 덮어쓰기).
 *
 * 메모리 (R007 D7)
 *  - 운영체제 수치가 아니라 직접 센다: main 엔트리 = 키 + 값 + DB_ENTRY_OVERHEAD, expires 엔트리 = 키 + 8 + DB_ENTRY_OVERHEAD.
 *  - maxmemory 가 0 이면 무제한.
 *  - db_make_room(need): 쓰기 전에 부른다. used + need > maxmemory 이면
 *      DB_NOEVICTION    → -1 (호출자는 OOM 에러를 응답)
 *      DB_ALLKEYS_RANDOM → 무작위 키를 내보내며 맞춘다. need 자체가 maxmemory 보다 크면 아무것도 지우지 않고 -1. */

#include <stddef.h>
#include <stdint.h>
#include "hashtable.h"

#define DB_ENTRY_OVERHEAD 64

enum { DB_NOEVICTION = 0, DB_ALLKEYS_RANDOM = 1 };

typedef struct {
    dict *main;
    dict *expires;  /* 키 → 만료 시각(int64_t, 8바이트) */
    size_t used;    /* 직접 센 메모리 */
    size_t maxmemory;
    int policy;
    size_t evicted; /* 지금까지 내보낸 키 수 */
} db;

db *db_create(size_t maxmemory, int policy);
void db_free(db *d);

entry *db_lookup(db *d, const char *k, size_t kl);                       /* lazy 만료 */
int db_key_expired(db *d, const char *k, size_t kl);                     /* TTL 이 있고 지났으면 1. 지우지 않음, main 안 봄 */
int db_set(db *d, const char *k, size_t kl, const char *v, size_t vl);   /* 0, 할당 실패 -1. TTL 제거 */
int db_del(db *d, const char *k, size_t kl);                             /* 지웠으면 1 (만료된 키는 0) */
int db_set_expire(db *d, const char *k, size_t kl, int64_t when_ms);    /* 키 없으면 0. when <= now 이면 즉시 삭제하고 1 */
int64_t db_ttl_ms(db *d, const char *k, size_t kl);                      /* -2 없음, -1 TTL 없음, 그 외 남은 ms(0 이상) */
int db_persist(db *d, const char *k, size_t kl);                         /* TTL 을 지웠으면 1 */
void db_flush(db *d);
size_t db_size(const db *d);                                             /* main 의 키 수(만료됐지만 아직 안 지운 키 포함) */

size_t db_entry_cost(size_t kl, size_t vl);
size_t db_used_memory(const db *d);
int db_make_room(db *d, size_t need);

#endif
