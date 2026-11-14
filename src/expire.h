#ifndef MINI_REDIS_EXPIRE_H
#define MINI_REDIS_EXPIRE_H

#include <stddef.h>
#include <stdint.h>

#include "db.h"

enum {
    EXPIRE_SAMPLE_COUNT = 20,
    EXPIRE_REPEAT_PERCENT = 25,
    EXPIRE_TIME_CHECK_INTERVAL = 16,
};

/* expire_cycle() 한 번의 결과 */
struct expire_stats {
    size_t sample_rounds;
    size_t deleted_count;
};

/* TTL 키 뽑기를 최대 EXPIRE_SAMPLE_COUNT 번 시도해 만료된 것을 지우고, 지운 수를 반환한다. */
size_t expire_sample(struct db *db, int64_t now_ms);

/* 만료 비율이 EXPIRE_REPEAT_PERCENT% 이상인 동안 샘플링을 반복한다.
 * budget_ms 를 넘기면 멈추며, 예산이 0 이어도 한 번은 샘플링한다. */
struct expire_stats expire_cycle(struct db *db, int64_t now_ms, int64_t budget_ms);

#endif
