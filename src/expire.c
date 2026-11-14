#include "expire.h"

#include <stdbool.h>

#include "clock.h"

enum {
    PERCENT_SCALE = 100,
};

static size_t sample_keys(struct db *db, int64_t now_ms, size_t *out_sampled_count);
static bool should_repeat(size_t sampled_count, size_t deleted_count);

size_t expire_sample(struct db *db, int64_t now_ms)
{
    size_t sampled_count = 0;
    return sample_keys(db, now_ms, &sampled_count);
}

/* 이벤트 루프를 오래 막지 않도록 예산을 두고, 시계 읽기 비용을 줄이려고 16번에 한 번만 확인한다.
 * 첫 번째 샘플 직후에도 확인해서 예산 0 이면 한 번만 돈다 */
struct expire_stats expire_cycle(struct db *db, int64_t now_ms, int64_t budget_ms)
{
    struct expire_stats stats = {0};
    int64_t start_ms = clock_now_ms();
    for (;;) {
        size_t sampled_count = 0;
        size_t deleted_count = sample_keys(db, now_ms, &sampled_count);
        stats.deleted_count += deleted_count;
        bool is_check_round = stats.sample_rounds % EXPIRE_TIME_CHECK_INTERVAL == 0;
        stats.sample_rounds++;

        if (!should_repeat(sampled_count, deleted_count)) {
            break;
        }
        if (is_check_round && clock_now_ms() - start_ms >= budget_ms) {
            break;
        }
    }
    return stats;
}

/* 뽑기를 최대 EXPIRE_SAMPLE_COUNT 번 시도한다. 같은 키가 두 번 뽑힐 수 있다(복원 추출).
 * 대량 삭제 직후 테이블이 성기면 뽑기가 빗나갈 수 있어, 빗나감도 시도 한 번으로 세고 계속한다 */
static size_t sample_keys(struct db *db, int64_t now_ms, size_t *out_sampled_count)
{
    size_t deleted_count = 0;
    size_t sampled_count = 0;
    for (size_t attempt = 0; attempt < EXPIRE_SAMPLE_COUNT && db_ttl_count(db) > 0; attempt++) {
        const struct str *key = db_random_ttl_key(db);
        if (key == NULL) {
            continue;
        }
        sampled_count++;
        if (db_delete_if_expired(db, key, now_ms) == DB_OK) {
            deleted_count++;
        }
    }
    *out_sampled_count = sampled_count;
    return deleted_count;
}

/* 뽑은 키 중 만료 비율이 기준 이상이면 아직 만료 키가 많이 남았다고 보고 다시 돈다 */
static bool should_repeat(size_t sampled_count, size_t deleted_count)
{
    if (sampled_count == 0) {
        return false;
    }
    return deleted_count * PERCENT_SCALE >= sampled_count * EXPIRE_REPEAT_PERCENT;
}
